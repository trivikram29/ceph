// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_rest_realm.h"
#include "rgw_rest_s3.h"
#include "rgw_rest_config.h"

#define dout_subsys ceph_subsys_rgw

// base period op, shared between Get and Post
class RGWOp_Period_Base : public RGWRESTOp {
 protected:
  RGWPeriod period;
 public:
  int verify_permission() override { return 0; }
  void send_response() override;
};

// reply with the period object on success
void RGWOp_Period_Base::send_response()
{
  set_req_state_err(s, http_ret);
  dump_errno(s);
  end_header(s);

  if (http_ret < 0)
    return;

  encode_json("period", period, s->formatter);
  flusher.flush();
}

// GET /admin/realm/period
class RGWOp_Period_Get : public RGWOp_Period_Base {
 public:
  void execute() override;
  const string name() override { return "get_period"; }
};

void RGWOp_Period_Get::execute()
{
  string realm_id, realm_name, period_id;
  epoch_t epoch = 0;
  RESTArgs::get_string(s, "realm_id", realm_id, &realm_id);
  RESTArgs::get_string(s, "realm_name", realm_name, &realm_name);
  RESTArgs::get_string(s, "period_id", period_id, &period_id);
  RESTArgs::get_uint32(s, "epoch", 0, &epoch);

  period.set_id(period_id);
  period.set_epoch(epoch);

  http_ret = period.init(store->ctx(), store, realm_id, realm_name);
  if (http_ret < 0)
    dout(5) << "failed to read period" << dendl;
}

// POST /admin/realm/period
class RGWOp_Period_Post : public RGWOp_Period_Base {
 public:
  void execute() override;
  const string name() override { return "post_period"; }
};

void RGWOp_Period_Post::execute()
{
  // initialize the period without reading from rados
  period.init(store->ctx(), store, false);

  // decode the period from input
#define PERIOD_INPUT_MAX_LEN 4096
  bool empty;
  http_ret = rgw_rest_get_json_input(store->ctx(), s, period,
                                     PERIOD_INPUT_MAX_LEN, &empty);
  if (http_ret < 0) {
    derr << "failed to decode period" << dendl;
    return;
  }

  // TODO: require period.realm_id to match an existing realm

  // nobody is allowed to push to the master zone
  if (period.get_master_zone() == store->zone.get_id()) {
    dout(10) << "master zone rejecting period id=" << period.get_id()
        << " epoch=" << period.get_epoch() << dendl;
    http_ret = -EINVAL; // XXX: error code
    return;
  }

  auto &realm = store->realm;
  const auto &current_period = store->current_period;

  if (period.get_id() != current_period.get_id()) {
    // new period must follow current period
    if (period.get_predecessor() != current_period.get_id()) {
      dout(10) << "current period " << current_period.get_id()
          << " is not period " << period.get_id() << "'s predecessor" << dendl;
      // XXX: this indicates a race between successive period updates. we should
      // fetch this new period's predecessors until we have a full history, then
      // set the latest period as the realm's current_period
      http_ret = -ENOENT; // XXX: error code
      return;
    }

    // write the period to rados
    http_ret = period.store_info(false);
    if (http_ret < 0) {
      derr << "failed to store new period" << dendl;
      return;
    }

    dout(4) << "current period " << current_period.get_id()
        << " is period " << period.get_id() << "'s predecessor, "
        "updating current period and notifying zone" << dendl;

    realm.set_current_period(period.get_id());
    // TODO: notify zone for dynamic reconfiguration
    return;
  }

  if (period.get_epoch() <= current_period.get_epoch()) {
    dout(10) << "period epoch " << period.get_epoch() << " is not newer "
        "than current epoch " << current_period.get_epoch()
        << ", discarding update" << dendl;
    http_ret = -EEXIST; // XXX: error code
    return;
  }

  // write the period to rados
  http_ret = period.store_info(false);
  if (http_ret < 0) {
    derr << "failed to store period " << period.get_id() << dendl;
    return;
  }

  dout(4) << "period epoch " << period.get_epoch() << " is newer "
      "than current epoch " << current_period.get_epoch()
      << ", updating latest epoch and notifying zone" << dendl;

  period.set_latest_epoch(period.get_epoch());

  http_ret = period.store_info(false);
  if (http_ret < 0) {
    derr << "failed to store period " << period.get_id() << dendl;
    return;
  }
  // TODO: notify zone for dynamic reconfiguration
}

class RGWHandler_Period : public RGWHandler_Auth_S3 {
 protected:
  RGWOp *op_get() override { return new RGWOp_Period_Get; }
  RGWOp *op_post() override { return new RGWOp_Period_Post; }
};

class RGWRESTMgr_Period : public RGWRESTMgr {
 public:
  RGWHandler* get_handler(struct req_state*) override {
    return new RGWHandler_Period;
  }
};


// GET /admin/realm
class RGWOp_Realm_Get : public RGWRESTOp {
  std::unique_ptr<RGWRealm> realm;
public:
  int verify_permission() override { return 0; }
  void execute() override;
  void send_response() override;
  const string name() { return "get_realm"; }
};

void RGWOp_Realm_Get::execute()
{
  string id;
  RESTArgs::get_string(s, "id", id, &id);
  string name;
  RESTArgs::get_string(s, "name", name, &name);

  // read realm
  realm.reset(new RGWRealm(id, name));
  http_ret = realm->init(g_ceph_context, store);
  if (http_ret < 0)
    derr << "failed to read realm id=" << id << " name=" << name << dendl;
}

void RGWOp_Realm_Get::send_response()
{
  set_req_state_err(s, http_ret);
  dump_errno(s);
  end_header(s);

  if (http_ret < 0)
    return;

  encode_json("realm", *realm, s->formatter);
  flusher.flush();
}

class RGWHandler_Realm : public RGWHandler_Auth_S3 {
protected:
  RGWOp *op_get() { return new RGWOp_Realm_Get; }
};

RGWRESTMgr_Realm::RGWRESTMgr_Realm()
{
  // add the /admin/realm/period resource
  register_resource("period", new RGWRESTMgr_Period);
}

RGWHandler* RGWRESTMgr_Realm::get_handler(struct req_state*)
{
  return new RGWHandler_Realm;
}
