// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2022 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include "rgw_sal_d4n.h"

#define dout_subsys ceph_subsys_rgw
#define dout_context g_ceph_context

#define MIN_MULITPART_SIZE 10

namespace rgw { namespace sal {

static inline Bucket* nextBucket(Bucket* t)
{
  if (!t)
    return nullptr;

  return dynamic_cast<FilterBucket*>(t)->get_next();
}

static inline Object* nextObject(Object* t)
{
  if (!t)
    return nullptr;
  
  return dynamic_cast<FilterObject*>(t)->get_next();
}

int D4NFilterDriver::initialize(CephContext *cct, const DoutPrefixProvider *dpp)
{
  FilterDriver::initialize(cct, dpp);
  blockDir->init(cct);
  d4nCache->init(cct);
  
  return 0;
}

std::unique_ptr<User> D4NFilterDriver::get_user(const rgw_user &u)
{
  std::unique_ptr<User> user = next->get_user(u);

  return std::make_unique<D4NFilterUser>(std::move(user), this);
}

std::unique_ptr<Object> D4NFilterBucket::get_object(const rgw_obj_key& k)
{
  std::unique_ptr<Object> o = next->get_object(k);

  return std::make_unique<D4NFilterObject>(std::move(o), this, driver);
}

int D4NFilterUser::create_bucket(const DoutPrefixProvider* dpp,
                              const rgw_bucket& b,
                              const std::string& zonegroup_id,
                              rgw_placement_rule& placement_rule,
                              std::string& swift_ver_location,
                              const RGWQuotaInfo * pquota_info,
                              const RGWAccessControlPolicy& policy,
                              Attrs& attrs,
                              RGWBucketInfo& info,
                              obj_version& ep_objv,
                              bool exclusive,
                              bool obj_lock_enabled,
                              bool* existed,
                              req_info& req_info,
                              std::unique_ptr<Bucket>* bucket_out,
                              optional_yield y)
{
  std::unique_ptr<Bucket> nb;
  int ret;

  ret = next->create_bucket(dpp, b, zonegroup_id, placement_rule, swift_ver_location, pquota_info, policy, attrs, info, ep_objv, exclusive, obj_lock_enabled, existed, req_info, &nb, y);
  if (ret < 0)
    return ret;

  Bucket* fb = new D4NFilterBucket(std::move(nb), this, driver);
  bucket_out->reset(fb);
  return 0;
}

int D4NFilterObject::copy_object(User* user,
                              req_info* info,
                              const rgw_zone_id& source_zone,
                              rgw::sal::Object* dest_object,
                              rgw::sal::Bucket* dest_bucket,
                              rgw::sal::Bucket* src_bucket,
                              const rgw_placement_rule& dest_placement,
                              ceph::real_time* src_mtime,
                              ceph::real_time* mtime,
                              const ceph::real_time* mod_ptr,
                              const ceph::real_time* unmod_ptr,
                              bool high_precision_time,
                              const char* if_match,
                              const char* if_nomatch,
                              AttrsMod attrs_mod,
                              bool copy_if_newer,
                              Attrs& attrs,
                              RGWObjCategory category,
                              uint64_t olh_epoch,
                              boost::optional<ceph::real_time> delete_at,
                              std::string* version_id,
                              std::string* tag,
                              std::string* etag,
                              void (*progress_cb)(off_t, void *),
                              void* progress_data,
                              const DoutPrefixProvider* dpp,
                              optional_yield y)
{
  /* Build cache block copy */
  rgw::d4n::CacheBlock* copyCacheBlock = new rgw::d4n::CacheBlock();

  copyCacheBlock->hostsList.push_back(driver->get_cache_block()->hostsList[0]); 
  copyCacheBlock->size = driver->get_cache_block()->size;
  copyCacheBlock->cacheObj.bucketName = dest_bucket->get_name();
  copyCacheBlock->cacheObj.objName = dest_object->get_key().get_oid();
  
  int copy_valueReturn = driver->get_block_dir()->copy_value(driver->get_cache_block(), copyCacheBlock);

  if (copy_valueReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Directory copy operation failed." << dendl;
  } else {
    ldpp_dout(dpp, 20) << "D4N Filter: Directory copy operation succeeded." << dendl;
  }

  delete copyCacheBlock;

  /* Append additional metadata to attributes */
  rgw::sal::Attrs baseAttrs = this->get_attrs();
  buffer::list bl;

  bl.append(to_iso_8601(*mtime));
  baseAttrs.insert({"mtime", bl});
  bl.clear();
  
  if (version_id != NULL) { 
    bl.append(*version_id);
    baseAttrs.insert({"version_id", bl});
    bl.clear();
  }
 
  if (!etag->empty()) {
    bl.append(*etag);
    baseAttrs.insert({"etag", bl});
    bl.clear();
  }

  if (attrs_mod == rgw::sal::ATTRSMOD_REPLACE) { /* Replace */
    rgw::sal::Attrs::iterator iter;

    for (const auto& pair : attrs) {
      iter = baseAttrs.find(pair.first);
    
      if (iter != baseAttrs.end()) {
        iter->second = pair.second;
      } else {
        baseAttrs.insert({pair.first, pair.second});
      }
    }
  } else if (attrs_mod == rgw::sal::ATTRSMOD_MERGE) { /* Merge */
    baseAttrs.insert(attrs.begin(), attrs.end()); 
  }

  int copy_attrsReturn = driver->get_d4n_cache()->copy_attrs(this->get_key().get_oid(), dest_object->get_key().get_oid(), &baseAttrs);

  if (copy_attrsReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache copy attributes operation failed." << dendl;
  } else {
    if (driver->get_cache_policy()->should_cache(this->get_obj_size(), MIN_MULITPART_SIZE)) {
      int copy_dataReturn = driver->get_d4n_cache()->copy_data(this->get_key().get_oid(), dest_object->get_key().get_oid());
 
      if (copy_dataReturn < 0) {
        ldpp_dout(dpp, 20) << "D4N Filter: Cache copy data operation failed." << dendl;
      } else {
        ldpp_dout(dpp, 20) << "D4N Filter: Cache copy object operation succeeded." << dendl;
      }
    }
  }

  return next->copy_object(user, info, source_zone,
                           nextObject(dest_object),
                           nextBucket(dest_bucket),
                           nextBucket(src_bucket),
                           dest_placement, src_mtime, mtime,
                           mod_ptr, unmod_ptr, high_precision_time, if_match,
                           if_nomatch, attrs_mod, copy_if_newer, attrs,
                           category, olh_epoch, delete_at, version_id, tag,
                           etag, progress_cb, progress_data, dpp, y);
}

int D4NFilterObject::set_obj_attrs(const DoutPrefixProvider* dpp, Attrs* setattrs,
                            Attrs* delattrs, optional_yield y) 
{
  if (setattrs != NULL) {
    /* Ensure setattrs and delattrs do not overlap */
    if (delattrs != NULL) {
      for (const auto& attr : *delattrs) {
        if (std::find(setattrs->begin(), setattrs->end(), attr) != setattrs->end()) {
          delattrs->erase(std::find(delattrs->begin(), delattrs->end(), attr));
        }
      }
    }

    int update_attrsReturn = driver->get_d4n_cache()->set_attrs(this->get_key().get_oid(), setattrs);

    if (update_attrsReturn < 0) {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache set object attributes operation failed." << dendl;
    } else {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache set object attributes operation succeeded." << dendl;
    }
  }

  if (delattrs != NULL) {
    std::vector<std::string> delFields;
    Attrs::iterator attrs;

    /* Extract fields from delattrs */
    for (attrs = delattrs->begin(); attrs != delattrs->end(); ++attrs) {
      delFields.push_back(attrs->first);
    }

    Attrs currentattrs = this->get_attrs();
    std::vector<std::string> currentFields;
    
    /* Extract fields from current attrs */
    for (attrs = currentattrs.begin(); attrs != currentattrs.end(); ++attrs) {
      currentFields.push_back(attrs->first);
    }
    
    int del_attrsReturn = driver->get_d4n_cache()->del_attrs(this->get_key().get_oid(), currentFields, delFields);

    if (del_attrsReturn < 0) {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache delete object attributes operation failed." << dendl;
    } else {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache delete object attributes operation succeeded." << dendl;
    }
  }

  return next->set_obj_attrs(dpp, setattrs, delattrs, y);  
}

int D4NFilterObject::get_obj_attrs(optional_yield y, const DoutPrefixProvider* dpp,
                                rgw_obj* target_obj)
{
  rgw::sal::Attrs newAttrs;
  std::vector< std::pair<std::string, std::string> > newMetadata;
  int get_attrsReturn = driver->get_d4n_cache()->get_attrs(this->get_key().get_oid(), 
						  &newAttrs, 
						  &newMetadata);

  if (get_attrsReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache get object attributes operation failed." << dendl;

    return next->get_obj_attrs(y, dpp, target_obj);
  } else {
    int set_attrsReturn = this->set_attrs(newAttrs);
    
    if (set_attrsReturn < 0) {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache get object attributes operation failed." << dendl;

      return next->get_obj_attrs(y, dpp, target_obj);
    } else {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache get object attributes operation succeeded." << dendl;
  
      return 0;
    }
  }
}

int D4NFilterObject::modify_obj_attrs(const char* attr_name, bufferlist& attr_val,
                               optional_yield y, const DoutPrefixProvider* dpp) 
{
  Attrs update;
  update[(std::string)attr_name] = attr_val;
  int update_attrsReturn = driver->get_d4n_cache()->update_attr(this->get_key().get_oid(), &update);

  if (update_attrsReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache modify object attribute operation failed." << dendl;
  } else {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache modify object attribute operation succeeded." << dendl;
  }

  return next->modify_obj_attrs(attr_name, attr_val, y, dpp);  
}

int D4NFilterObject::delete_obj_attrs(const DoutPrefixProvider* dpp, const char* attr_name,
                               optional_yield y) 
{
  std::vector<std::string> delFields;
  delFields.push_back((std::string)attr_name);
  
  Attrs::iterator attrs;
  Attrs currentattrs = this->get_attrs();
  std::vector<std::string> currentFields;
  
  /* Extract fields from current attrs */
  for (attrs = currentattrs.begin(); attrs != currentattrs.end(); ++attrs) {
    currentFields.push_back(attrs->first);
  }
  
  int delAttrReturn = driver->get_d4n_cache()->del_attrs(this->get_key().get_oid(), currentFields, delFields);

  if (delAttrReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache delete object attribute operation failed." << dendl;
  } else {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache delete object attribute operation succeeded." << dendl;
  }
  
  return next->delete_obj_attrs(dpp, attr_name, y);  
}

std::unique_ptr<Object> D4NFilterDriver::get_object(const rgw_obj_key& k)
{
  std::unique_ptr<Object> o = next->get_object(k);

  return std::make_unique<D4NFilterObject>(std::move(o), this);
}

std::unique_ptr<Writer> D4NFilterDriver::get_atomic_writer(const DoutPrefixProvider *dpp,
				  optional_yield y,
				  rgw::sal::Object* obj,
				  const rgw_user& owner,
				  const rgw_placement_rule *ptail_placement_rule,
				  uint64_t olh_epoch,
				  const std::string& unique_tag)
{
  std::unique_ptr<Writer> writer = next->get_atomic_writer(dpp, y, nextObject(obj),
							   owner, ptail_placement_rule,
							   olh_epoch, unique_tag);

  return std::make_unique<D4NFilterWriter>(std::move(writer), this, obj, dpp, true);
}

std::unique_ptr<Object::ReadOp> D4NFilterObject::get_read_op()
{
  std::unique_ptr<ReadOp> r = next->get_read_op();
  return std::make_unique<D4NFilterReadOp>(std::move(r), this);
}

std::unique_ptr<Object::DeleteOp> D4NFilterObject::get_delete_op()
{
  std::unique_ptr<DeleteOp> d = next->get_delete_op();
  return std::make_unique<D4NFilterDeleteOp>(std::move(d), this);
}

int D4NFilterObject::D4NFilterReadOp::prepare(optional_yield y, const DoutPrefixProvider* dpp)
{
  int getDirReturn = source->driver->get_block_dir()->get_value(source->driver->get_cache_block());

  if (getDirReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Directory get operation failed." << dendl;
  } else {
    ldpp_dout(dpp, 20) << "D4N Filter: Directory get operation succeeded." << dendl;
  }

  rgw::sal::Attrs newAttrs;
  std::vector< std::pair<std::string, std::string> > newMetadata;
  int getObjReturn = source->driver->get_d4n_cache()->get_attrs(source->get_key().get_oid(), 
							&newAttrs, 
							&newMetadata);

  int ret = next->prepare(y, dpp);
  
  if (getObjReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache get object operation failed." << dendl;
  } else {
    /* Set metadata locally */
    RGWQuotaInfo quota_info;
    RGWObjState* astate;
    source->get_obj_state(dpp, &astate, y);

    for (auto it = newMetadata.begin(); it != newMetadata.end(); ++it) {
      if (!std::strcmp(it->first.data(), "mtime")) {
        parse_time(it->second.data(), &astate->mtime); 
      } else if (!std::strcmp(it->first.data(), "object_size")) {
	source->set_obj_size(std::stoull(it->second));
      } else if (!std::strcmp(it->first.data(), "accounted_size")) {
	astate->accounted_size = std::stoull(it->second);
      } else if (!std::strcmp(it->first.data(), "epoch")) {
	astate->epoch = std::stoull(it->second);
      } else if (!std::strcmp(it->first.data(), "version_id")) {
	source->set_instance(it->second);
      } else if (!std::strcmp(it->first.data(), "source_zone_short_id")) {
	astate->zone_short_id = static_cast<uint32_t>(std::stoul(it->second));
      } else if (!std::strcmp(it->first.data(), "bucket_count")) {
	source->get_bucket()->set_count(std::stoull(it->second));
      } else if (!std::strcmp(it->first.data(), "bucket_size")) {
	source->get_bucket()->set_size(std::stoull(it->second));
      } else if (!std::strcmp(it->first.data(), "user_quota.max_size")) {
        quota_info.max_size = std::stoull(it->second);
      } else if (!std::strcmp(it->first.data(), "user_quota.max_objects")) {
        quota_info.max_objects = std::stoull(it->second);
      } else if (!std::strcmp(it->first.data(), "max_buckets")) {
        source->get_bucket()->get_owner()->set_max_buckets(std::stoull(it->second));
      }
    }

    source->get_bucket()->get_owner()->set_info(quota_info);
    source->set_obj_state(*astate);
   
    /* Set attributes locally */
    int set_attrsReturn = source->set_attrs(newAttrs);

    if (set_attrsReturn < 0) {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache get object operation failed." << dendl;
    } else {
      ldpp_dout(dpp, 20) << "D4N Filter: Cache get object operation succeeded." << dendl;
    }   
  }

  return ret;
}

int D4NFilterObject::D4NFilterDeleteOp::delete_obj(const DoutPrefixProvider* dpp,
					   optional_yield y)
{
  int delDirReturn = source->driver->get_block_dir()->del_value(source->driver->get_cache_block());

  if (delDirReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Directory delete operation failed." << dendl;
  } else {
    ldpp_dout(dpp, 20) << "D4N Filter: Directory delete operation succeeded." << dendl;
  }

  Attrs::iterator attrs;
  Attrs currentattrs = source->get_attrs();
  std::vector<std::string> currentFields;
  
  /* Extract fields from current attrs */
  for (attrs = currentattrs.begin(); attrs != currentattrs.end(); ++attrs) {
    currentFields.push_back(attrs->first);
  }

  int delObjReturn = source->driver->get_d4n_cache()->del_object(source->get_key().get_oid());

  if (delObjReturn < 0) {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache delete object operation failed." << dendl;
  } else {
    ldpp_dout(dpp, 20) << "D4N Filter: Cache delete operation succeeded." << dendl;
  }

  return next->delete_obj(dpp, y);
}

int D4NFilterWriter::prepare(optional_yield y) 
{
  /* Set caching policy */
  shouldCache = driver->get_cache_policy()->should_cache("PUT");

  if (shouldCache) {
    int del_dataReturn = driver->get_d4n_cache()->del_data(obj->get_key().get_oid());

    if (del_dataReturn < 0) {
      ldpp_dout(save_dpp, 20) << "D4N Filter: Cache delete data operation failed." << dendl;
    } else {
      ldpp_dout(save_dpp, 20) << "D4N Filter: Cache delete data operation succeeded." << dendl;
    }
  }

  return next->prepare(y);
}

int D4NFilterWriter::process(bufferlist&& data, uint64_t offset)
{
  bufferlist objectData = data;
  int append_dataReturn = driver->get_d4n_cache()->append_data(obj->get_key().get_oid(), data);

  if (append_dataReturn < 0) {
    ldpp_dout(save_dpp, 20) << "D4N Filter: Cache append data operation failed." << dendl;
  } else {
    ldpp_dout(save_dpp, 20) << "D4N Filter: Cache append data operation succeeded." << dendl;
  }

  return next->process(std::move(data), offset);
}

int D4NFilterWriter::complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
                       std::map<std::string, bufferlist>& attrs,
                       ceph::real_time delete_at,
                       const char *if_match, const char *if_nomatch,
                       const std::string *user_data,
                       rgw_zone_set *zones_trace, bool *canceled,
                       optional_yield y)
{
  rgw::d4n::CacheBlock* tempCacheBlock = driver->get_cache_block();
  rgw::d4n::BlockDirectory* tempBlockDir = driver->get_block_dir();

  tempCacheBlock->hostsList.push_back(tempBlockDir->get_host() + ":" + std::to_string(tempBlockDir->get_port())); 
  tempCacheBlock->size = accounted_size;
  tempCacheBlock->cacheObj.bucketName = obj->get_bucket()->get_name();
  tempCacheBlock->cacheObj.objName = obj->get_key().get_oid();

  int setDirReturn = tempBlockDir->set_value(tempCacheBlock);

  if (setDirReturn < 0) {
    ldpp_dout(save_dpp, 20) << "D4N Filter: Directory set operation failed." << dendl;
  } else {
    ldpp_dout(save_dpp, 20) << "D4N Filter: Directory set operation succeeded." << dendl;
  }
   
  /* Retrieve complete set of attrs */
  int ret = next->complete(accounted_size, etag, mtime, set_mtime, attrs,
			delete_at, if_match, if_nomatch, user_data, zones_trace,
			canceled, y);

  if (shouldCache) {
    obj->get_obj_attrs(y, save_dpp, NULL);

    /* Append additional metadata to attributes */ 
    rgw::sal::Attrs baseAttrs = obj->get_attrs();
    rgw::sal::Attrs attrs_temp = baseAttrs;
    buffer::list bl;
    RGWObjState* astate;
    obj->get_obj_state(save_dpp, &astate, y);

    bl.append(to_iso_8601(obj->get_mtime()));
    baseAttrs.insert({"mtime", bl});
    bl.clear();

    bl.append(std::to_string(obj->get_obj_size()));
    baseAttrs.insert({"object_size", bl});
    bl.clear();

    bl.append(std::to_string(accounted_size));
    baseAttrs.insert({"accounted_size", bl});
    bl.clear();
   
    bl.append(std::to_string(astate->epoch));
    baseAttrs.insert({"epoch", bl});
    bl.clear();

    if (obj->have_instance()) {
      bl.append(obj->get_instance());
      baseAttrs.insert({"version_id", bl});
      bl.clear();
    } else {
      bl.append(""); /* Empty value */
      baseAttrs.insert({"version_id", bl});
      bl.clear();
    }

    auto iter = attrs_temp.find(RGW_ATTR_SOURCE_ZONE);
    if (iter != attrs_temp.end()) {
      bl.append(std::to_string(astate->zone_short_id));
      baseAttrs.insert({"source_zone_short_id", bl});
      bl.clear();
    } else {
      bl.append("0"); /* Initialized to zero */
      baseAttrs.insert({"source_zone_short_id", bl});
      bl.clear();
    }

    bl.append(std::to_string(obj->get_bucket()->get_count()));
    baseAttrs.insert({"bucket_count", bl});
    bl.clear();

    bl.append(std::to_string(obj->get_bucket()->get_size()));
    baseAttrs.insert({"bucket_size", bl});
    bl.clear();

    RGWUserInfo info = obj->get_bucket()->get_owner()->get_info();
    bl.append(std::to_string(info.quota.user_quota.max_size));
    baseAttrs.insert({"user_quota.max_size", bl});
    bl.clear();

    bl.append(std::to_string(info.quota.user_quota.max_objects));
    baseAttrs.insert({"user_quota.max_objects", bl});
    bl.clear();

    bl.append(std::to_string(obj->get_bucket()->get_owner()->get_max_buckets()));
    baseAttrs.insert({"max_buckets", bl});
    bl.clear();

    baseAttrs.insert(attrs.begin(), attrs.end());

    int set_attrsReturn = driver->get_d4n_cache()->set_attrs(obj->get_key().get_oid(), &baseAttrs);

    if (set_attrsReturn < 0) {
      ldpp_dout(save_dpp, 20) << "D4N Filter: Cache set attributes operation failed." << dendl;
    } else {
      ldpp_dout(save_dpp, 20) << "D4N Filter: Cache set attributes operation succeeded." << dendl;
    }
  }
  
  return ret;
}

} } // namespace rgw::sal

extern "C" {

rgw::sal::Driver* newD4NFilter(rgw::sal::Driver* next)
{
  rgw::sal::D4NFilterDriver* driver = new rgw::sal::D4NFilterDriver(next);

  return driver;
}

}
