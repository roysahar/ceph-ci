/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2023 IBM, Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#include "mds/QuiesceDbManager.h"
#include "common/debug.h"
#include "fmt/format.h"
#include "include/ceph_assert.h"
#include <algorithm>
#include <random>
#include <ranges>
#include <type_traits>
#include "boost/url.hpp"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds_quiesce
#undef dout_prefix
#define dout_prefix *_dout << "quiesce.mgr <" << __func__ << "> "

#undef dout
#define dout(lvl)                                                        \
  do {                                                                   \
    auto subsys = ceph_subsys_mds;                                       \
    if ((dout_context)->_conf->subsys.should_gather(dout_subsys, lvl)) { \
      subsys = dout_subsys;                                              \
    }                                                                    \
  dout_impl(dout_context, ceph::dout::need_dynamic(subsys), lvl) dout_prefix

#undef dendl
#define dendl \
  dendl_impl; \
  }           \
  while (0)

#define dset(suffix) "[" << set_id << "@" << set.db_version << "] " << suffix
#define dsetroot(suffix) "[" << set_id << "@" << set.db_version << "," << root << "] " << suffix

static QuiesceTimeInterval time_distance(QuiesceTimePoint lhs, QuiesceTimePoint rhs) {
  if (lhs > rhs) {
    return lhs - rhs;
  } else {
    return rhs - lhs;
  }
}

void* QuiesceDbManager::quiesce_db_thread_main()
{
  db_thread_enter();

  std::unique_lock ls(submit_mutex);
  QuiesceTimeInterval next_event_at_age = QuiesceTimeInterval::max();
  auto last_acked = 0;

  while (true) {

    auto db_age = db.get_age();

    if (!db_thread_has_work() && next_event_at_age > db_age) {
      submit_condition.wait_for(ls, next_event_at_age - db_age);
    }

    if (!membership_upkeep()) {
      break;
    }

    decltype(pending_acks) acks(std::move(pending_acks));
    decltype(pending_requests) requests(std::move(pending_requests));
    decltype(pending_db_update) db_update;

    db_update.swap(pending_db_update);

    ls.unlock();

    if (membership.leader == membership.me) {
      next_event_at_age = leader_upkeep(acks, requests);
    } else if (db_update) {
      next_event_at_age = replica_upkeep(*db_update);
    }
  
    complete_requests();

    // by default, only send ack if the version has changed
    bool send_ack = last_acked != db.version;
    QuiesceMap quiesce_map(db.version);
    {
      std::lock_guard lc(agent_mutex);
      if (agent_callback) {
        if (agent_callback->if_newer < db.version) {
          dout(20) << "notifying agent with db version " << db.version << dendl;
          calculate_quiesce_map(quiesce_map);
          send_ack = agent_callback->notify(quiesce_map);
          agent_callback->if_newer = db.version;
        } else {
          send_ack = false;
        }
      } else {
        // by default, ack the db version and agree to whatever was sent
        // This means that a quiesce cluster member with an empty agent callback 
        // will cause roots to stay quiescing indefinitely
        dout(5) << "no agent callback registered, responding with an empty ack" << dendl;
      }
    }

    if (send_ack) {
      dout(20) << "sending ack with " << quiesce_map.roots.size() << " roots for db version " << quiesce_map.db_version << dendl;
      auto rc = membership.send_ack(quiesce_map);
      if (rc != 0) {
        dout(1) << "ERROR ("<< rc <<") when sending agent ack for version " 
        << quiesce_map.db_version << " with "<< quiesce_map.roots.size() << " roots" << dendl;
      } else {
        last_acked = quiesce_map.db_version;
      }
    }

    ls.lock();
  }

  ls.unlock();

  db_thread_exit();

  return 0;
}

bool QuiesceDbManager::membership_upkeep()
{
  if (cluster_membership && cluster_membership->epoch == membership.epoch) {
    // no changes
    return true;
  }

  bool was_leader = membership.epoch > 0 && membership.leader == membership.me;
  bool is_leader = cluster_membership && cluster_membership->leader == cluster_membership->me;
  if (cluster_membership) {
    dout(10) << "epoch: " << cluster_membership->epoch << " is_leader: " << is_leader << " was_leader: " << was_leader << dendl;
  } else {
    dout(10) << "shutdown! was_leader: " << was_leader << dendl;
  }

  if (is_leader) {
    // remove peers that aren't present anymore
    for (auto peer_it = peers.begin(); peer_it != peers.end();) {
      if (cluster_membership->members.contains(peer_it->first)) {
        peer_it++;
      } else {
        peer_it = peers.erase(peer_it);
      }
    }
    // create empty info for new peers
    for (auto peer : cluster_membership->members) {
      peers.try_emplace(peer);
    }
    if (!was_leader) {
      // the leader has changed. 
      // TODO: should we increment the version of every active set in this case?
    }
  } else {
    peers.clear();
    // abort awaits with EINPROGRESS
    // the reason is that we don't really have a new version
    // of any of the sets, we just aren't authoritative anymore
    // hence, EINPROGRESS is a more appropriate response than, say, EINTR
    for (auto & [_, await_ctx]: awaits) {
      done_requests[await_ctx.req_ctx] = EINPROGRESS;
    }
    awaits.clear();
    // reject pending requests
    while (!pending_requests.empty()) {
      done_requests[pending_requests.front()] = EPERM;
      pending_requests.pop();
    }
  }

  if (cluster_membership) {
    membership = *cluster_membership;
  }

  return cluster_membership.has_value();
}

QuiesceTimeInterval QuiesceDbManager::replica_upkeep(QuiesceDbListing& update)
{
  if (update.epoch != membership.epoch) {
    dout(10) << "ignoring db update from another epoch: " << update.epoch << " != " << membership.epoch << dendl;
    return QuiesceTimeInterval::max();
  }

  auto time_zero = QuiesceClock::now() - update.db_age;
  if (time_distance(time_zero, db.time_zero) > std::chrono::seconds(1)) {
    dout(10) << "significant db_time_zero change to " << time_zero << " from " << db.time_zero << dendl;
  }
  db.time_zero = time_zero;

  if (db.version > update.db_version) {
    dout(3) << "got an older version of DB from the leader: " << db.version << " > " << update.db_version << dendl;
    dout(3) << "discarding the DB" << dendl;
    db.reset();
  } else {
    for (auto& [qs_id, qs] : update.sets) {
      // TODO: emplace didn't work
      db.sets[qs_id] = std::move(qs);
    }
    db.version = update.db_version;
  }

  // wait forever
  return QuiesceTimeInterval::max();
}

QuiesceTimeInterval QuiesceDbManager::leader_upkeep(decltype(pending_acks)& acks, decltype(pending_requests)& requests) {
  if (db.version == 0) {
    db.time_zero = QuiesceClock::now();
    db.sets.clear();
  }

  // record peer acks
  while (!acks.empty()) {
    auto& [from, diff_map] = acks.front();
    leader_record_ack(from, diff_map);
    acks.pop();
  }

  // process requests
  while (!requests.empty()) {
    auto req_ctx = requests.front();
    int result = leader_process_request(req_ctx);
    if (result != EBUSY) {
      done_requests[req_ctx] = result;
    }
    requests.pop();
  }

  QuiesceTimeInterval next_db_event_at_age = leader_upkeep_db();
  QuiesceTimeInterval next_await_event_at_age = leader_upkeep_awaits();

  return std::min(next_db_event_at_age, next_await_event_at_age);
}

void QuiesceDbManager::complete_requests() {
  for (auto [req, res]: done_requests) {
    auto & r = req->response;
    r.clear();
    if (membership.leader == membership.me) {
      r.db_age = db.get_age();
      r.db_version = db.version;
      r.epoch = membership.epoch;

      if (req->request.set_id) {
        Db::Sets::const_iterator it = db.sets.find(*req->request.set_id);
        if (it != db.sets.end()) {
          r.sets.emplace(*it);
        }
      } else if (req->request.is_query()) {
        for (auto && it : std::as_const(db.sets)) {
          r.sets.emplace(it);
        }
      }
    }
    // non-zero result codes are all errors
    req->complete(-res);
  }
  done_requests.clear();
}

void QuiesceDbManager::leader_record_ack(mds_rank_t from, QuiesceMap& diff_map)
{
  auto it = peers.find(from);

  if (it == peers.end()) {
    // ignore updates from unknown peers
    return;
  }

  auto & info = it->second;

  if (diff_map.db_version > db.version) {
    dout(3) << "ignoring unknown version ack by rank " << from << " (" << diff_map.db_version << " > " << db.version << ")" << dendl;
    dout(5) << "will send the peer a full DB" << dendl;
    info.diff_map.reset();
  } else {
    info.diff_map = std::move(diff_map);
    info.at_age = db.get_age();
  }
}

static std::string random_hex_string() {
  std::mt19937 gen(std::random_device {} ());
  return fmt::format("{:x}", gen());
}

bool QuiesceDbManager::sanitize_roots(QuiesceDbRequest::Roots& roots)
{
  static const std::string file_scheme = "file";
  static const std::string inode_scheme = "inode";
  static const std::unordered_set<std::string> supported_schemes { file_scheme, inode_scheme };
  QuiesceDbRequest::Roots result;
  for (auto &root : roots) {
    auto parsed_uri = boost::urls::parse_uri_reference(root);
    if (!parsed_uri) {
      dout(2) << "Couldn't parse root '" << root << "' as URI (error: " << parsed_uri.error() << ")" << dendl;
      return false;
    }

    boost::url root_url = parsed_uri.value();
    root_url.normalize();

    if (!root_url.has_scheme()) {
      root_url.set_scheme(file_scheme);
    } else if (!supported_schemes.contains(root_url.scheme())) {
      dout(2) << "Unsupported root URL scheme '" << root_url.scheme() << "'" << dendl;
      return false;
    }

    if (root_url.has_authority()) {
      auto auth_str = root_url.authority().buffer();
      bool ok_remove = false;
      if (auth_str == membership.fs_name) {
        ok_remove = true;
      } else {
        try {
          ok_remove = std::stoll(auth_str) == membership.fs_id;
        } catch (...) { }
      }
      if (ok_remove) {
        // OK, but remove the authority for now
        // we may want to enforce it if we decide to keep a single database for all file systems
        dout(10) << "Removing the fs name or id '" << auth_str << "' from the root url authority section" << dendl;
        root_url.remove_authority();
      } else {
        dout(2) << "The root url '" << root_url.buffer() 
          << "' includes an authority section '" << auth_str 
          << "' which doesn't match the fs id (" << membership.fs_id 
          << ") or name ('" << membership.fs_name << "')" << dendl;
        return false;
      }
    }

    std::string sanitized_path;
    sanitized_path.reserve(root_url.path().size());
    // deal with the file path
    //  * make it absolute (start with a slash)
    //  * remove repeated slashes
    //  * remove the trailing slash
    bool skip_slash = true;
    for (auto&& c : root_url.path()) {
      if (c != '/' || !skip_slash) {
        sanitized_path.push_back(c);
      }
      skip_slash = c == '/';
    }

    if (sanitized_path.size() > 0 && sanitized_path.back() == '/') {
      sanitized_path.pop_back();
    }

    if (root_url.scheme() == file_scheme) {
      sanitized_path.insert(sanitized_path.begin(), '/');
    } else if (root_url.scheme() == inode_scheme) {
      uint64_t inodeno = 0;
      try {
        inodeno = std::stoull(sanitized_path);
      } catch (...) { }

      if (!inodeno || fmt::format("{}", inodeno) != sanitized_path) {
        dout(2) << "Root '" << root << "' does not encode a vaild inode number" << dendl;
        return false;
      }
    }

    root_url.set_path(sanitized_path);

    if (root_url.buffer() != root) {
      dout(10) << "Normalized root '" << root << "' to '" << root_url.buffer() << "'" << dendl;
    }
    result.insert(root_url.buffer());
  }
  roots.swap(result);
  return true;
}

int QuiesceDbManager::leader_process_request(RequestContext* req_ctx)
{
  QuiesceDbRequest &request = req_ctx->request;

  if (!request.is_valid()) {
    dout(2) << "rejecting an invalid request" << dendl;
    return EINVAL;
  }

  if (!sanitize_roots(request.roots)) {
    dout(2) << "failed to sanitize roots for a request" << dendl;
    return EINVAL;
  }

  const auto db_age = db.get_age();

  if (request.is_cancel_all()) {
    dout(3) << "WARNING: got a cancel all request" << dendl;
    // special case - reset all
    // this will only work on active sets
    for (auto &[set_id, set]: db.sets) {
      if (set.is_active()) {
        bool did_update = false;
        for (auto&& [_, member]: set.members) {
          did_update |= !member.excluded;
          member.excluded = true;
        }

        ceph_assert(did_update);
        ceph_assert(set.rstate.update(QS_CANCELED, db_age));
        set.db_version = db.version+1;
      }
    }
    return 0;
  }

  // figure out the set to update
  auto set_it = db.sets.end();

  if (request.set_id) {
    set_it = db.sets.find(*request.set_id);
  } else if (request.if_version > 0) {
    dout(2) << "can't expect a non-zero version (" << *request.if_version << ") for a new set" << dendl;
    return EINVAL;
  }

  if (set_it == db.sets.end()) {
    if (request.includes_roots() && request.if_version <= 0) {
      // such requests may introduce a new set
      if (!request.set_id) {
        // we should generate a unique set id
        QuiesceSetId new_set_id;
        do {
          new_set_id = random_hex_string();
        } while (db.sets.contains(new_set_id));
        // update the set_id in the request so that we can
        // later know which set got created
        request.set_id.emplace(std::move(new_set_id));
      }
      set_it = db.sets.emplace(*request.set_id, QuiesceSet()).first;
    } else if (request.is_mutating() || request.await) {
      ceph_assert(request.set_id.has_value());
      dout(2) << "coudn't find set with id '" << *request.set_id <<  "'" << dendl;
      return ENOENT;
    }
  }

  if (set_it != db.sets.end()) {
    auto& [set_id, set] = *set_it;

    int result = leader_update_set(*set_it, request);
    if (result != 0) {
      return result;
    }

    if (request.await) {
      // this check may have a false negative for a quiesced set
      // that will be released in another request in the same batch
      // in that case, this await will be enqueued but then found and completed
      // with the same error in `leader_upkeep_awaits`
      if ((set.is_releasing() || set.is_released()) && !request.is_release()) {
        dout(2) << dset("can't quiesce-await a set that was released (") << set.rstate.state << ")" << dendl;
        return EPERM;
      }

      auto expire_at_age = interval_saturate_add(db_age, *request.await);
      awaits.emplace(std::piecewise_construct,
          std::forward_as_tuple(set_id),
          std::forward_as_tuple(expire_at_age, req_ctx));
      // let the caller know that the request isn't done yet
      return EBUSY;
    }
  }

  // if we got here it must be a success
  return 0;
}

int QuiesceDbManager::leader_update_set(Db::Sets::value_type& set_it, const QuiesceDbRequest& request)
{
  auto & [set_id, set] = set_it;
  if (request.if_version && set.db_version != *request.if_version) {
    dout(10) << dset("is newer than requested (") << *request.if_version << ") " << dendl;
    return ESTALE;
  }

  if (!request.is_mutating()) {
    return 0;
  }

  bool did_update = false;
  bool did_update_roots = false;

  if (request.is_release()) {
    // the release command is allowed in states
    // quiesced, releasing, released
    switch (set.rstate.state) {
      case QS_QUIESCED:
        // we only update the state to RELEASING,
        // and not the age. This is to keep counting
        // towards the quiesce expiration.
        // TODO: this could be reconsidered, but would
        // then probably require an additional timestamp
        set.rstate.state = QS_RELEASING;
        did_update = true;
        dout(15) << dset("") << "updating state to: " << set.rstate.state << dendl;
      case QS_RELEASING:
      case QS_RELEASED:
        break;
      default:
        dout(2) << dset("can't release in the state: ") << set.rstate.state << dendl;
        return EPERM;
    }
  } else {
    const auto db_age = db.get_age();
    bool reset = false;

    if (!request.is_reset()) {
      // only active or new sets can be modified
      if (!set.is_active() && set.db_version > 0) {
        dout(2) << dset("rejecting modification in the terminal state: ") << set.rstate.state << dendl;
        return EPERM;
      } else if (request.includes_roots() && set.is_releasing()) {
        dout(2) << dset("rejecting new roots in the QS_RELEASING state") << dendl;
        return EPERM;
      }
    } else {
      // a reset request can be used to resurrect a set from whichever state it's in now
      if (set.rstate.state > QS_QUIESCED) {
        dout(5) << dset("reset back to a QUIESCING state") << dendl;
        did_update = set.rstate.update(QS_QUIESCING, db_age);
        ceph_assert(did_update);
        reset = true;
      }
    }

    if (request.timeout) {
      set.timeout = *request.timeout;
      did_update = true;
    }

    if (request.expiration) {
      set.expiration = *request.expiration;
      did_update = true;
    }

    size_t included_count = 0;
    QuiesceState min_member_state = QS__MAX;

    for (auto& [root, info] : set.members) {
      if (request.should_exclude(root)) {
        did_update_roots |= !info.excluded;
        info.excluded = true;
      } else if (!info.excluded) {
        included_count ++;

        QuiesceState effective_member_state;

        if (reset) {
          dout(5) << dsetroot("reset back to a QUIESCING state") << dendl;
          info.rstate.state = QS_QUIESCING;
          info.rstate.at_age = db_age;
          did_update_roots = true;
          effective_member_state = info.rstate.state;
        } else {
          QuiesceState min_reported_state;
          QuiesceState max_reported_state;
          size_t reporting_peers = check_peer_reports(set_id, set, root, info, min_reported_state, max_reported_state);

          if (reporting_peers == peers.size() && max_reported_state < QS__FAILURE) {
            effective_member_state = min_reported_state;
          } else {
            effective_member_state = info.rstate.state;
          }
        }

        min_member_state = std::min(min_member_state, effective_member_state);
      }
    }

    if (request.includes_roots()) {
      for (auto const& root : request.roots) {
        auto const& [member_it, emplaced] = set.members.try_emplace(root, db_age);
        auto& [_, info] = *member_it;
        if (emplaced || info.excluded) {
          info.excluded = false;
          did_update_roots = true;
          included_count++;
          info.rstate = { QS_QUIESCING, db_age };
          min_member_state = std::min(min_member_state, QS_QUIESCING);
        }
      }
    }

    did_update |= did_update_roots;

    if (included_count == 0) {
      dout(20) << dset("cancelled due to 0 included members") << dendl;
      did_update = set.rstate.update(QS_CANCELED, db_age);
      ceph_assert(did_update);
    } else if (min_member_state < QS__MAX) {
      auto next_state = set.next_state(min_member_state);
      if (did_update |= set.rstate.update(next_state, db_age)) {
        dout(15) << dset("updated to match the min state of the remaining (") << included_count << ") members: " << set.rstate.state << dendl;
      }
    }
  }

  if (did_update) {
    dout(20) << dset("updating version from ") << set.db_version << " to " << db.version + 1 << dendl;
    set.db_version = db.version + 1;
    if (did_update_roots) {
      // any awaits pending on this set must be interrupted
      // NB! even though the set may be QUIESCED now, it could only
      // get there due to exclusion of quiescing roots, which is
      // not a vaild way to successfully await a set, hence EINTR
      // However, if the set had all roots removed then we
      // should respond in ECANCELED to notify that no more await
      // attempts will be permitted
      auto range = awaits.equal_range(set_id);
      int rc = EINTR;
      if (!set.is_active()) {
        ceph_assert(set.rstate.state == QS_CANCELED);
        rc = ECANCELED;
      }
      for (auto it = range.first; it != range.second; it++) {
        done_requests[it->second.req_ctx] = rc;
      }
      if (range.first != range.second) {
        dout(10) << dset("interrupting awaits with rc = ") << rc << " due to a change in members" << dendl;
      }
      awaits.erase(range.first, range.second);
    }
  }

  return 0;
}

QuiesceTimeInterval QuiesceDbManager::leader_upkeep_db()
{
  std::map<mds_rank_t, std::deque<std::reference_wrapper<Db::Sets::value_type>>> peer_updates;

  QuiesceTimeInterval next_event_at_age = QuiesceTimeInterval::max();
  QuiesceDbVersion max_version = db.version;

  for(auto & set_it: db.sets) {
    auto & [set_id, set] = set_it;
    auto next_set_event_at_age = leader_upkeep_set(set_it);

    max_version = std::max(max_version, set.db_version);
    next_event_at_age = std::min(next_event_at_age, next_set_event_at_age);

    for(auto const & [peer, info]: peers) {
      // update remote peers (not myself) if their version is lower
      if (peer != membership.me && info.diff_map.db_version < set.db_version) {
        peer_updates[peer].emplace_back(set_it);
      }
    }
  }

  db.version = max_version;

  // update the peers
  for (auto &[peer, sets]: peer_updates) {
    QuiesceDbListing update;
    update.epoch = membership.epoch;
    update.db_age = db.get_age();
    update.db_version = db.version;
    for (auto const& setpair : sets) {
      update.sets.insert(setpair);
    }

    dout(20) << "updating peer " << peer << " with " << update.sets.size() 
      << " sets modified in db version range (" 
      << peers[peer].diff_map.db_version << ".." << db.version << "]" << dendl;

    auto rc = membership.send_listing_to(peer, update);
    if (rc != 0) {
      dout(1) << "ERROR (" << rc << ") trying to replicate db version " 
        << update.db_version << " with " << update.sets.size() 
        << " sets to the peer " << peer << dendl;
    }
  }

  return next_event_at_age;
}

QuiesceState QuiesceSet::next_state(QuiesceState min_member_state) const {
  ceph_assert(min_member_state > QS__INVALID);
  ceph_assert(rstate.state < QS__TERMINAL);

  if (is_releasing() && min_member_state == QS_QUIESCED) {
    // keep releasing
    return QS_RELEASING;
  }

  // otherwise, follow the member state
  return min_member_state;
}

size_t QuiesceDbManager::check_peer_reports(const QuiesceSetId& set_id, const QuiesceSet& set, const QuiesceRoot& root, const QuiesceSet::MemberInfo& member, QuiesceState& min_reported_state, QuiesceState& max_reported_state) {
  min_reported_state = QS__MAX;
  max_reported_state = QS__INVALID;

  size_t reporting_peers = 0;

  for (auto& [peer, info] : peers) {
    if (info.diff_map.db_version >= set.db_version) {
      // we get here only if we've seen the peer
      // ack a version >= set.db_version
      auto dit = info.diff_map.roots.find(root);
      if (dit != info.diff_map.roots.end()) {
        // the peer has something to say about this root
        auto const& pr_state = dit->second;
        if (!pr_state.is_valid()) {
          dout(5) << dsetroot("ignoring an invalid peer state ") << pr_state.state << dendl;
          continue;
        }
        min_reported_state = std::min(min_reported_state, set.get_effective_member_state(pr_state.state));
        max_reported_state = std::max(max_reported_state, set.get_effective_member_state(pr_state.state));
      } else {
        // no diff for this root from the peer,
        // we assume that the peer agrees with our state
        min_reported_state = std::min(min_reported_state, set.get_effective_member_state(member.rstate.state));
        max_reported_state = std::max(max_reported_state, set.get_effective_member_state(member.rstate.state));
      }
      reporting_peers++;
    }
  }

  return reporting_peers;
}

QuiesceTimeInterval QuiesceDbManager::leader_upkeep_set(Db::Sets::value_type& set_it)
{
  auto& [set_id, set] = set_it;

  if (!set.is_active()) {
    return QuiesceTimeInterval::max();
  }

  QuiesceTimeInterval end_of_life = QuiesceTimeInterval::max();

  const auto db_age = db.get_age();
  // no quiescing could have started before the current db_age

  QuiesceState min_member_state = QS__MAX;
  size_t included_members = 0;
  // for each included member, apply recorded acks and check quiesce timeouts
  for (auto& [root, member] : set.members) {
    if (member.excluded) {
      continue;
    }
    included_members++;

    QuiesceState min_reported_state;
    QuiesceState max_reported_state;

    size_t reporting_peers = check_peer_reports(set_id, set, root, member, min_reported_state, max_reported_state);

    if (max_reported_state >= QS__FAILURE) {
      // if at least one peer is reporting a failure state then move to it
      dout(5) << dsetroot("reported by at least one peer as: ") << max_reported_state << dendl;
      if (member.rstate.update(max_reported_state, db_age)) {
        dout(15) << dsetroot("updating member state to ") << member.rstate.state << dendl;
        set.db_version = db.version + 1;
      }
    } else if (min_reported_state < member.rstate.state) {
      // someone has reported a rollback state for the root
      dout(15) << dsetroot("reported by at least one peer as ") << min_reported_state << " vs. the expected " << member.rstate.state << dendl;
      if (member.rstate.update(min_reported_state, db_age)) {
        dout(15) << dsetroot("updating member state to ") << member.rstate.state << dendl;
        set.db_version = db.version + 1;
      }
    } else if (reporting_peers == peers.size()) {
      dout(20) << dsetroot("min state for all (") << reporting_peers << ") peers: " << min_reported_state << dendl;
      if (member.rstate.update(min_reported_state, db_age)) {
        dout(15) << dsetroot("updating member state to ") << member.rstate.state << dendl;
        set.db_version = db.version + 1;
      }
    }

    if (member.is_quiescing()) {
      // the quiesce timeout applies in this case
      auto timeout_at_age = interval_saturate_add(member.rstate.at_age, set.timeout);
      if (timeout_at_age <= db_age) {
        // NB: deliberately not changing the member state
        dout(10) << dsetroot("detected a member quiesce timeout") << dendl;
        ceph_assert(set.rstate.update(QS_TIMEDOUT, db_age));
        set.db_version = db.version + 1;
        break;
      }
      end_of_life = std::min(end_of_life, timeout_at_age);
    } else if (member.is_failed()) {
      // if at least one member is in a failure state
      // then the set must receive it as well
      dout(5) << dsetroot("propagating the terminal member state to the set level: ") << member.rstate.state << dendl;
      ceph_assert(set.rstate.update(member.rstate.state, db_age));
      set.db_version = db.version + 1;
      break;
    }

    min_member_state = std::min(min_member_state, member.rstate.state);
  }

  if (!set.is_active()) {
    return QuiesceTimeInterval::max();
  }

  // we should have at least one included members to be active
  ceph_assert(included_members > 0);
  auto next_state = set.next_state(min_member_state);

  if (set.rstate.update(next_state, db_age)) {
    set.db_version = db.version + 1;
    dout(15) << dset("updated set state to match member reports: ") << set.rstate.state << dendl;
  }

  if (set.is_quiesced() || set.is_released()) {
    // any awaits pending on this set should be completed now,
    // before the set may enter a QS_EXPIRED state
    // due to a zero expiration timeout.
    // this could be used for barriers.
    auto range = awaits.equal_range(set_id);
    for (auto it = range.first; it != range.second; it++) {
      done_requests[it->second.req_ctx] = 0;
      if (set.is_quiesced()) {
        // since we've just completed a _quiesce_ await
        // we should also reset the recorded age of the QUIESCED state
        // to postpone the expiration time checked below
        set.rstate.at_age = db_age;
        set.db_version = db.version + 1;
        dout(20) << dset("reset quiesced state age upon successful await") << dendl;
      }
    }
    awaits.erase(range.first, range.second);
  }

  // check timeouts:
  if (set.is_quiescing()) {
    // sanity check that we haven't missed this before
    ceph_assert(end_of_life > db_age);
  } else if (set.is_active()) {
    auto expire_at_age = interval_saturate_add(set.rstate.at_age, set.expiration);
    if (expire_at_age <= db_age) {
      // we have expired
      ceph_assert(set.rstate.update(QS_EXPIRED, db_age));
      set.db_version = db.version + 1;
    } else {
      end_of_life = std::min(end_of_life, expire_at_age);
    }
  }

  return end_of_life;
}

QuiesceTimeInterval QuiesceDbManager::leader_upkeep_awaits()
{
  QuiesceTimeInterval next_event_at_age = QuiesceTimeInterval::max();
  for (auto it = awaits.begin(); it != awaits.end();) {
    auto & [set_id, actx] = *it;
    Db::Sets::const_iterator set_it = db.sets.find(set_id);

    int rc = db.get_age() >= actx.expire_at_age ? EINPROGRESS : EBUSY;

    if (set_it == db.sets.cend()) {
      rc = ENOENT;
    } else {
      auto const & set = set_it->second;

      switch(set.rstate.state) {
        case QS_CANCELED:
          rc = ECANCELED;
          break;
        case QS_EXPIRED:
        case QS_TIMEDOUT:
          rc = ETIMEDOUT; 
          break;
        case QS_QUIESCED:
          rc = 0; // fallthrough
        case QS_QUIESCING:
          ceph_assert(!actx.req_ctx->request.is_release());
          break;
        case QS_RELEASED:
          rc = 0; // fallthrough
        case QS_RELEASING:
          if (!actx.req_ctx->request.is_release()) {
            // technically possible for a quiesce await
            // to get here if a concurrent release request
            // was submitted in the same batch;
            // see the corresponding check in
            // `leader_process_request`
            rc = EPERM;
          }
          break;
        default: ceph_abort("unexpected quiesce set state");
      }
    }

    if (rc != EBUSY) {
      dout(10) << "completing an await for the set '" << set_id << "' with rc: " << rc << dendl;
      done_requests[actx.req_ctx] = rc;
      it = awaits.erase(it);
    } else {
      next_event_at_age = std::min(next_event_at_age, actx.expire_at_age);
      ++it;
    }
  }
  return next_event_at_age;
}

static QuiesceTimeInterval get_root_ttl(const QuiesceSet & set, const QuiesceSet::MemberInfo &member, QuiesceTimeInterval db_age) {

  QuiesceTimeInterval end_of_life = db_age;

  if (set.is_quiesced() || set.is_releasing()) {
    end_of_life = set.rstate.at_age + set.expiration;
  } else if (set.is_active()) {
    auto age = db_age; // taking the upper bound here
    if (member.is_quiescing()) {
      // we know that this member is on a timer
      age = member.rstate.at_age;
    }
    end_of_life = age + set.timeout; 
  }

  if (end_of_life > db_age) {
    return end_of_life - db_age;
  } else {
    return QuiesceTimeInterval::zero();
  }
}

void QuiesceDbManager::calculate_quiesce_map(QuiesceMap &map)
{
  map.roots.clear();
  map.db_version = db.version;
  auto db_age = db.get_age();

  for(auto & [set_id, set]: db.sets) {
    if (set.is_active()) {
      // we only report active sets;
      for(auto & [root, member]: set.members) {
        if (member.excluded) {
          continue;
        }
        auto effective_member_state = set.get_effective_member_state(member.rstate.state);
        auto ttl = get_root_ttl(set, member, db_age);
        auto root_it = map.roots.try_emplace(root, QuiesceMap::RootInfo { effective_member_state, ttl }).first;

        // the min below resolves conditions when members representing the same root have different state/ttl
        // e.g. if at least one member is QUIESCING then the root should be QUIESCING
        root_it->second.state = std::min(root_it->second.state, effective_member_state);
        root_it->second.ttl = std::min(root_it->second.ttl, ttl);
      }
    }
  }
}
