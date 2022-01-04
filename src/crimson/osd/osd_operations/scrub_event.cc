// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <seastar/core/future.hh>

#include <boost/smart_ptr/local_shared_ptr.hpp>

#include "common/Formatter.h"
#include "crimson/osd/osd.h"
#include "crimson/osd/osd_operations/scrub_event.h"
#include "crimson/osd/pg.h"
#include "messages/MOSDPGLog.h"

namespace {
seastar::logger& logger()
{
  return crimson::get_logger(ceph_subsys_osd);
}
}  // namespace


namespace crimson::osd {

// --------------------------- ScrubEvent ---------------------------

ScrubEvent::ScrubEvent(Ref<PG> pg,
                       ShardServices& shard_services,
                       const spg_t& pgid,
                       ScrubEventFwd func,
                       epoch_t epoch_queued,
                       Scrub::act_token_t tkn,
                       std::chrono::milliseconds delay)
    : pg{std::move(pg)}
    , event_fwd_func{func}
    , act_token{tkn}
    , shard_services{shard_services}
    , pgid{pgid}
    , epoch_queued{epoch_queued}
    , delay{delay}
    , dbg_desc{"<ScrubEvent>"}
{
  logger().debug("ScrubEvent: 1'st ctor {:p} {} delay:{}", (void*)this,
                 dbg_desc, delay);
}

ScrubEvent::ScrubEvent(Ref<PG> pg,
                       ShardServices& shard_services,
                       const spg_t& pgid,
                       ScrubEventFwd func,
                       epoch_t epoch_queued,
                       Scrub::act_token_t tkn)
    : ScrubEvent{std::move(pg),
                 shard_services,
                 pgid,
                 func,
                 epoch_queued,
                 tkn,
                 std::chrono::milliseconds{0}}
{
  logger().debug("ScrubEvent: 2'nd ctor {:p} {}", (void*)this, dbg_desc);
}

ScrubEvent::ScrubEvent(nullevent_tag_t,
                       Ref<PG> pg,
                       ShardServices& shard_services,
                       const spg_t& pgid,
                       ScrubEventFwd func)
    : ScrubEvent{std::move(pg),
                 shard_services,
                 pgid,
                 func,
                 epoch_queued,
                 0,
                 std::chrono::milliseconds{0}}
{
  logger().debug("ScrubEvent: dummy event");
}

void ScrubEvent::print(std::ostream& lhs) const
{
  lhs << fmt::format("{}", *this);
}

void ScrubEvent::dump_detail(Formatter* f) const
{
  f->open_object_section("ScrubEvent");
  // f->dump_stream("from") << from;
  f->dump_stream("pgid") << pgid;
  // f->dump_int("sent", evt.get_epoch_sent());
  // f->dump_int("requested", evt.get_epoch_requested());
  // f->dump_string("evt", evt.get_desc());
  f->close_section();
}

void ScrubEvent::on_pg_absent()
{
  logger().warn("{}: pg absent, dropping", *this);
}

seastar::future<Ref<PG>> ScrubEvent::get_pg()
{
  return seastar::make_ready_future<Ref<PG>>(pg);
}

ScrubEvent::interruptible_future<> ScrubEvent::complete_rctx(Ref<PG> pg)
{
  logger().debug("{}: no ctx for now to submit", *this);
  //   if (pg) {
  //     return shard_services.dispatch_context(pg->get_collection_ref(),
  //     std::move(ctx));
  //   } else {
  //     return shard_services.dispatch_context_messages(std::move(ctx));
  //   }
  return seastar::make_ready_future<>();
}

ScrubEvent::PGPipeline& ScrubEvent::pp(PG& pg)
{
  return pg.scrub_event_pg_pipeline;
}


ScrubEvent::~ScrubEvent() = default;


// clang-format off
seastar::future<> ScrubEvent::start()
{
  logger().debug(
    "scrubber: ScrubEvent::start(): {}: start (delay: {}) pg:{:p}", *this,
    delay, (void*)&(*pg));

  IRef ref = this;
  auto maybe_delay = seastar::now();
  if (delay.count() > 0) {
    maybe_delay = seastar::sleep(delay);
  }

  return maybe_delay.then([this] {
    return get_pg();
  }).then([this](Ref<PG> pg) {
    return interruptor::with_interruption([this, pg]() -> ScrubEvent::interruptible_future<> {
      if (!pg) {
        logger().warn("scrubber: ScrubEvent::start(): {}: pg absent, did not create", *this);
        on_pg_absent();
        handle.exit();
        return complete_rctx(pg);
      }
      logger().debug("scrubber: ScrubEvent::start(): {}: pg present", *this);
      return with_blocking_future_interruptible<interruptor::condition>(
        handle.enter(pp(*pg).await_map)
      ).then_interruptible([this, pg] {
        return with_blocking_future_interruptible<interruptor::condition>(
          pg->osdmap_gate.wait_for_map(epoch_queued));
      }).then_interruptible([this, pg](auto) {
        return with_blocking_future_interruptible<interruptor::condition>(
          handle.enter(pp(*pg).local_sync));
      }).then_interruptible([this, pg]() {
        return with_blocking_future_interruptible<interruptor::condition>(
          handle.enter(pp(*pg).process));
      }).then_interruptible([this, pg]() mutable -> ScrubEvent::interruptible_future<>  {

        logger().info("ScrubEvent::start() {} executing...", *this);
        if (std::holds_alternative<ScrubEvent::ScrubEventFwdImm>(event_fwd_func)) {
          (*(pg->get_scrubber(Scrub::ScrubberPasskey{})).*std::get<ScrubEvent::ScrubEventFwdImm>(event_fwd_func))(epoch_queued);
          return seastar::make_ready_future<>();
        } else {
          return (*(pg->get_scrubber(Scrub::ScrubberPasskey{})).*std::get<ScrubEvent::ScrubEventFwdFut>(event_fwd_func))(epoch_queued);
        }

      }).then_interruptible([this, pg]() mutable {
        logger().info("ScrubEvent::start() {} after calling fwder", *this);
        handle.exit();
        logger().info("ScrubEvent::start() {} executing... exited", *this);
        return complete_rctx(pg);
      }).then_interruptible([pg]() -> ScrubEvent::interruptible_future<> {
        return seastar::now();
      });
    },
    [this](std::exception_ptr ep) {
      logger().debug("ScrubEvent::start(): {} interrupted with {}", *this, ep);
      return seastar::now();
    },
    pg);
  }).finally([this, ref=std::move(ref)] {
    logger().debug("ScrubEvent::start(): {} complete", *this /*, *ref*/);
  });
}
// clang-format on

ScrubEvent::rett ScrubEvent::lockout()
{
  return handle.enter(pp(*pg).local_sync);
}

void ScrubEvent::unlock()
{
  handle.exit();
}

// seastar::future<PipelineHandle> ScrubEvent::lockout(PG& pg)
// {
//   PipelineHandle hdl;
//
//   return handle.enter(ScrubEvent::pp(pg).local_sync
//     ).then_interruptible<void>([handle=std::move(handle)]() mutable {
//       return seastar::make_ready_future<PipelineHandle>(std::move(handle));
//     });

//      return seastar::make_ready_future<PipelineHandle>(std::move(handle));
//   })

//   return with_blocking_future<void>([&] {
//     return hdl.enter(ScrubEvent::pp(pg).local_sync);
//   }).then([&] {
//     return seastar::make_ready_future<PipelineHandle>(std::move(handle));
//   });
//
//   //      .then([handle=std::move(handle)]()/* ->
//   ScrubEvent::interruptible_future<>*/ {
//   //      return
//   seastar::make_ready_future<PipelineHandle>(std::move(handle));
//    //   });
//   return with_blocking_future<>(hdl.enter(ScrubEvent::pp(pg).local_sync))
//         .then([handle=std::move(hdl)]() -> seastar::future<PipelineHandle> {
//         return seastar::make_ready_future<PipelineHandle>(std::move(handle));
//       });
//}

// void ScrubEvent::unlock(PG& pg, PipelineHandle&& handle)
// {
//   handle.exit();
// }





// /////////////////////////////////////////////////////////////////////////////

// /////////////////////////////////////////////////////////////////////////////

// --------------------------- RemoteScrubEvent ---------------------------

RemoteScrubEvent::RemoteScrubEvent(
             reserve_req_tag_t, // tag
             OSD& osd,
             ShardServices& shard_services,
             const spg_t& pgid,
             int req_type, // MOSDScrubReserve::type
             pg_shard_t from,

             epoch_t map_epoch,
             //Scrub::act_token_t tkn,
             std::chrono::milliseconds delay)
    : /*pg{std::move(pg)}
    , event_fwd_func{func}
    , act_token{tkn}
    , */
        osd{osd}
    , shard_services{shard_services}
    , pgid{pgid}
    , map_epoch{map_epoch}
    , delay{delay}
    , dbg_desc{"<RemoteScrubEvent>"}
{
  // locate the PG first?
  payload_msg = std::make_unique<MOSDScrubReserve>(
    pgid,
    map_epoch,
    req_type,
    from);


}

// RemoteScrubEvent::RemoteScrubEvent(Ref<PG> pg,
//                        ShardServices& shard_services,
//                        const spg_t& pgid,
//                        ScrubEventFwd func,
//                        epoch_t epoch_queued,
//                        Scrub::act_token_t tkn,
//                        std::chrono::milliseconds delay)
//     : /*pg{std::move(pg)}
//     ,*/ act_token{tkn}
//     , shard_services{shard_services}
//     , pgid{pgid}
//     , epoch_queued{epoch_queued}
//     , delay{delay}
//     , dbg_desc{"<RemoteScrubEvent>"}
// {
//   logger().debug("RemoteScrubEvent: 1'st ctor {:p} {} delay:{}", (void*)this,
//                  dbg_desc, delay);
// }


void RemoteScrubEvent::print(std::ostream& lhs) const
{
  lhs << fmt::format("{}", *this);
}

void RemoteScrubEvent::dump_detail(Formatter* f) const
{
  f->open_object_section("RemoteScrubEvent");
  // f->dump_stream("from") << from;
  f->dump_stream("pgid") << pgid;
  // f->dump_int("sent", evt.get_epoch_sent());
  // f->dump_int("requested", evt.get_epoch_requested());
  // f->dump_string("evt", evt.get_desc());
  f->close_section();
}

void RemoteScrubEvent::on_pg_absent()
{
  logger().warn("{}: pg absent, dropping", *this);
}

// seastar::future<Ref<PG>> RemoteScrubEvent::get_pg()
// {
//   return seastar::make_ready_future<Ref<PG>>(pg);
// }

RemoteScrubEvent::interruptible_future<> RemoteScrubEvent::complete_rctx(Ref<PG> pg)
{
  logger().debug("{}: no ctx for now to submit", *this);
  //   if (pg) {
  //     return shard_services.dispatch_context(pg->get_collection_ref(),
  //     std::move(ctx));
  //   } else {
  //     return shard_services.dispatch_context_messages(std::move(ctx));
  //   }
  return seastar::make_ready_future<>();
}

RemoteScrubEvent::PGPipeline& RemoteScrubEvent::pp(PG& pg)
{
  return pg.scrub_event_pg_pipeline;
}


RemoteScrubEvent::~RemoteScrubEvent() = default;

seastar::future<> RemoteScrubEvent::do_op(int msg_type)
{
  switch (msg_type) {
    case MSG_OSD_SCRUB_RESERVE:
      logger().error("{}: MSG_OSD_SCRUB_RESERVE", *this);
      return seastar::make_ready_future<>();

    default:
      logger().error("{}: unhandled message type {}", *this);
      return seastar::make_ready_future<>();
  }
}


// clang-format off
seastar::future<> RemoteScrubEvent::start()
{
  logger().info(
    "scrubber: RemoteScrubEvent::start(): {}: start (delay: {})", *this,
    delay);

  IRef opref = this;
  auto maybe_delay = seastar::now();
  if (delay.count() > 0) {
    maybe_delay = seastar::sleep(delay);
  }

  return maybe_delay.then([this, opref=std::move(opref)] {

    return with_blocking_future(osd.osdmap_gate.wait_for_map(payload_msg->get_min_epoch()))
    .then([this, opref=std::move(opref)] (epoch_t epoch) {
    return with_blocking_future(osd.wait_for_pg(payload_msg->get_spg()));
  }).then([this, opref=std::move(opref)] (Ref<PG> pgref) {
    return interruptor::with_interruption([this, opref, pgref] {
      return seastar::do_with(std::move(pgref), std::move(opref),
	[this](auto& pgref, auto& opref) mutable -> ScrubEvent::interruptible_future<>{
          return do_op(payload_msg->get_type());
	//return pgref->get_recovery_backend()->handle_recovery_op(m);
      });
    }, [](std::exception_ptr) { return seastar::now(); }, pgref);
  });
        });




#if 0
    return payload_msg->get_spg();
  }).then([this](Ref<PG> pg) {
    return interruptor::with_interruption([this, pg]() -> RemoteScrubEvent::interruptible_future<> {
      if (!pg) {
        logger().warn("scrubber: RemoteScrubEvent::start(): {}: pg absent, did not create", *this);
        on_pg_absent();
        handle.exit();
        return complete_rctx(pg);
      }
      logger().debug("scrubber: RemoteScrubEvent::start(): {}: pg present", *this);
      return with_blocking_future_interruptible<interruptor::condition>(
        handle.enter(pp(*pg).await_map)
      ).then_interruptible([this, pg] {
        return with_blocking_future_interruptible<interruptor::condition>(
          pg->osdmap_gate.wait_for_map(epoch_queued));
      }).then_interruptible([this, pg](auto) {
        return with_blocking_future_interruptible<interruptor::condition>(
          handle.enter(pp(*pg).local_sync));
      }).then_interruptible([this, pg]() {
        return with_blocking_future_interruptible<interruptor::condition>(
          handle.enter(pp(*pg).process));
      }).then_interruptible([this, pg]() mutable -> RemoteScrubEvent::interruptible_future<>  {

        logger().info("RemoteScrubEvent::start() {} executing...", *this);
        if (std::holds_alternative<RemoteScrubEvent::ScrubEventFwdImm>(event_fwd_func)) {
          (*(pg->get_scrubber(Scrub::ScrubberPasskey{})).*std::get<RemoteScrubEvent::ScrubEventFwdImm>(event_fwd_func))(epoch_queued);
          return seastar::make_ready_future<>();
        } else {
          return (*(pg->get_scrubber(Scrub::ScrubberPasskey{})).*std::get<RemoteScrubEvent::ScrubEventFwdFut>(event_fwd_func))(epoch_queued);
        }

      }).then_interruptible([this, pg]() mutable {
        logger().info("RemoteScrubEvent::start() {} after calling fwder", *this);
        handle.exit();
        logger().info("RemoteScrubEvent::start() {} executing... exited", *this);
        return complete_rctx(pg);
      }).then_interruptible([pg]() -> RemoteScrubEvent::interruptible_future<> {
        return seastar::now();
      });
    },
    [this](std::exception_ptr ep) {
      logger().debug("RemoteScrubEvent::start(): {} interrupted with {}", *this, ep);
      return seastar::now();
    },
    pg);
  }).finally([this, ref=std::move(ref)] {
    logger().debug("RemoteScrubEvent::start(): {} complete", *this /*, *ref*/);
  });
#endif


}
// clang-format on



}  // namespace crimson::osd

/*
seastar::future<> RecoverySubRequest::start() {
  logger().debug("{}: start", *this);

  IRef opref = this;
  return with_blocking_future(
      osd.osdmap_gate.wait_for_map(m->get_min_epoch()))
  .then([this] (epoch_t epoch) {
    return with_blocking_future(osd.wait_for_pg(m->get_spg()));
  }).then([this, opref=std::move(opref)] (Ref<PG> pgref) {
    return interruptor::with_interruption([this, opref, pgref] {
      return seastar::do_with(std::move(pgref), std::move(opref),
	[this](auto& pgref, auto& opref) {
	return pgref->get_recovery_backend()->handle_recovery_op(m);
      });
    }, [](std::exception_ptr) { return seastar::now(); }, pgref);
  });
}
*/
