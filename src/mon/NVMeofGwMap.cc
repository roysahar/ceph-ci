#include <boost/tokenizer.hpp>
#include "include/stringify.h"
#include "NVMeofGwMon.h"
#include "NVMeofGwMap.h"
#include "OSDMonitor.h"

using std::map;
using std::make_pair;
using std::ostream;
using std::ostringstream;
using std::string;

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix *_dout << "nvmeofgw " << __PRETTY_FUNCTION__ << " "

void NVMeofGwMap::to_gmap(std::map<GROUP_KEY, GWMAP>& Gmap) const {
    Gmap.clear();
    for (const auto& created_map_pair: Created_gws) {
        const auto& group_key = created_map_pair.first;
        const GW_CREATED_MAP& gw_created_map = created_map_pair.second;
        for (const auto& gw_created_pair: gw_created_map) {
            const auto& gw_id = gw_created_pair.first;
            const auto& gw_created  = gw_created_pair.second;

            auto gw_state = GW_STATE_T(gw_created.ana_grp_id, epoch);
            for (const auto& sub: gw_created.subsystems) {
                gw_state.subsystems.insert({sub.nqn, NqnState(sub.nqn, gw_created.sm_state, gw_created )});
            }
            Gmap[group_key][gw_id] = gw_state;
        }
    }
}

int  NVMeofGwMap::cfg_add_gw(const GW_ID_T &gw_id, const GROUP_KEY& group_key) {
    // Calculate allocated group bitmask
    bool allocated[MAX_SUPPORTED_ANA_GROUPS] = {false};
    for (auto& itr: Created_gws[group_key]) {
        allocated[itr.second.ana_grp_id] = true;
        if(itr.first == gw_id) {
            dout(4) << __func__ << " ERROR create GW: already exists in map " << gw_id << dendl;
            return -EEXIST ;
        }
    }

    // Allocate the new group id
    for(int i=0; i<=MAX_SUPPORTED_ANA_GROUPS; i++) {
        if (allocated[i] == false) {
            GW_CREATED_T gw_created(i);
            Created_gws[group_key][gw_id] = gw_created;
            dout(4) << __func__ << "Created GWS:  " << Created_gws  <<  dendl;
            return 0;
        }
    }

    dout(4) << __func__ << " ERROR create GW: " << gw_id << "   ANA groupId was not allocated "   << dendl;
    return -EINVAL;
}

int NVMeofGwMap::cfg_delete_gw(const GW_ID_T &gw_id, const GROUP_KEY& group_key) {
    int rc = 0;
    for (auto& gws_states: Created_gws[group_key]) {

        if (gws_states.first == gw_id) {
            auto& state = gws_states.second;
            for(int i=0; i<MAX_SUPPORTED_ANA_GROUPS; i++){
                bool modified;
                fsm_handle_gw_delete (gw_id, group_key, state.sm_state[i], i, modified);
            }
            dout(4) << " Delete GW :"<< gw_id  << " ANA grpid: " << state.ana_grp_id  << dendl;
            Gmetadata[group_key].erase(gw_id);
        } else {
            rc = -EINVAL;
        }
    }
    Created_gws[group_key].erase(gw_id);
    return rc;
}

void NVMeofGwMap::update_active_timers( bool &propose_pending ){

    //dout(4) << __func__  <<  " called,  p_monitor: " << mon << dendl;
    for (auto& group_md: Gmetadata) {
        auto& group_key = group_md.first;
        auto& pool = group_key.first;
        auto& group = group_key.second;

        for (auto& gw_md: group_md.second) {
            auto& gw_id = gw_md.first;
            auto& md = gw_md.second;
            for (int i = 0; i < MAX_SUPPORTED_ANA_GROUPS; i++) {
                if (md.data[i].anagrp_sm_tstamps == INVALID_GW_TIMER) continue;

                md.data[i].anagrp_sm_tstamps++;
                dout(4) << "timer for GW " << gw_id << " ANA GRP " << i<<" : " << md.data[i].anagrp_sm_tstamps << "value: "<< md.data[i].timer_value << dendl;
                if(md.data[i].anagrp_sm_tstamps >= md.data[i].timer_value){
                    fsm_handle_to_expired (gw_id, std::make_pair(pool, group), i, propose_pending);
                }
            }
        }

    }
}


int NVMeofGwMap::process_gw_map_gw_down(const GW_ID_T &gw_id, const GROUP_KEY& group_key,
                                            bool &propose_pending) {
    int rc = 0;
    auto& gws_states = Created_gws[group_key];
    auto  gw_state = gws_states.find(gw_id);
    if (gw_state != gws_states.end()) {
        dout(4) << "GW down " << gw_id << dendl;
        auto& st = gw_state->second;
        st.availability = GW_AVAILABILITY_E::GW_UNAVAILABLE;
        for (ANA_GRP_ID_T i = 0; i < MAX_SUPPORTED_ANA_GROUPS; i ++) {
            fsm_handle_gw_down (gw_id, group_key, st.sm_state[i], i, propose_pending);
            st.standby_state(i);
        }
    }
    else {
        dout(4)  << __FUNCTION__ << "ERROR GW-id was not found in the map " << gw_id << dendl;
        rc = -EINVAL;
    }
    return rc;
}


void NVMeofGwMap::process_gw_map_ka(const GW_ID_T &gw_id, const GROUP_KEY& group_key,  epoch_t& last_osd_epoch, bool &propose_pending)
{

#define     FAILBACK_PERSISTENCY_INT_SEC 8
    auto& gws_states = Created_gws[group_key];
    auto  gw_state = gws_states.find(gw_id);
    ceph_assert (gw_state != gws_states.end());
    auto& st = gw_state->second;
    dout(4)  << "KA beacon from the GW " << gw_id << " in state " << (int)st.availability << dendl;

    if (st.availability == GW_AVAILABILITY_E::GW_CREATED) {
        // first time appears - allow IO traffic for this GW
        st.availability = GW_AVAILABILITY_E::GW_AVAILABLE;
        for (int i = 0; i < MAX_SUPPORTED_ANA_GROUPS; i++) st.sm_state[i] = GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE;
        if (st.ana_grp_id != REDUNDANT_GW_ANA_GROUP_ID) { // not a redundand GW
            st.active_state(st.ana_grp_id);
        }
        propose_pending = true;
    }
    else if (st.availability == GW_AVAILABILITY_E::GW_UNAVAILABLE) {
        st.availability = GW_AVAILABILITY_E::GW_AVAILABLE;
        if (st.ana_grp_id == REDUNDANT_GW_ANA_GROUP_ID) {
            for (int i = 0; i < MAX_SUPPORTED_ANA_GROUPS; i++) st.sm_state[i] = GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE;
            propose_pending = true; //TODO  try to find the 1st GW overloaded by ANA groups and start  failback for ANA group that it is not an owner of
        }
        else {
            //========= prepare to Failback to this GW =========
            // find the GW that took over on the group st.ana_grp_id
            bool some_found = false;
            propose_pending = true;
            find_failback_gw(gw_id, group_key, some_found);
            if (!some_found ) { // There is start of single GW so immediately turn its group to GW_ACTIVE_STATE
                dout(4)  << "Warning - not found the GW responsible for" << st.ana_grp_id << " that took over the GW " << gw_id << "when it was fallen" << dendl;
                st.active_state(st.ana_grp_id);
            }
        }
    }
    else if (st.availability == GW_AVAILABILITY_E::GW_AVAILABLE) {
        for(int i=0; i<MAX_SUPPORTED_ANA_GROUPS; i++){
          fsm_handle_gw_alive (gw_id, group_key, gw_state->second, st.sm_state[i], i, last_osd_epoch, propose_pending);
        }
    }
}


void NVMeofGwMap::handle_abandoned_ana_groups(bool& propose)
{
    propose = false;
    for (auto& group_state: Created_gws) {
        auto& group_key = group_state.first;
        auto& gws_states = group_state.second;

            for (auto& gw_state : gws_states) { // loop for GWs inside nqn group
                auto& gw_id = gw_state.first;
                GW_CREATED_T& state = gw_state.second;

                //1. Failover missed : is there is a GW in unavailable state? if yes, is its ANA group handled by some other GW?
                if (state.availability == GW_AVAILABILITY_E::GW_UNAVAILABLE && state.ana_grp_id != REDUNDANT_GW_ANA_GROUP_ID) {
                    auto found_gw_for_ana_group = false;
                    for (auto& gw_state2 : gws_states) {
                        GW_CREATED_T& state2 = gw_state2.second;
                        if (state2.availability == GW_AVAILABILITY_E::GW_AVAILABLE && state2.sm_state[state.ana_grp_id] == GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE) {
                            found_gw_for_ana_group = true; // dout(4) << "Found GW " << ptr2.first << " that handles ANA grp " << (int)state->optimized_ana_group_id << dendl;
                            break;
                        }
                    }
                    if (found_gw_for_ana_group == false) { //choose the GW for handle ana group
                        dout(4)<< "Was not found the GW " << " that handles ANA grp " << (int)state.ana_grp_id << " find candidate "<< dendl;

                        for (int i = 0; i < MAX_SUPPORTED_ANA_GROUPS; i++)
                            find_failover_candidate( gw_id, group_key, i, propose );
                    }
                }

                //2. Failback missed: Check this GW is Available and Standby and no other GW is doing Failback to it
                else if (state.availability == GW_AVAILABILITY_E::GW_AVAILABLE
                            && state.ana_grp_id != REDUNDANT_GW_ANA_GROUP_ID &&
                            state.sm_state[state.ana_grp_id] == GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE)
                {
                    bool found = false;
                    for (auto& gw_state2 : gws_states) {
                        auto& state2 = gw_state2.second;
                        if (state2.sm_state[state.ana_grp_id] == GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED){
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        dout(4) << __func__ << " GW " << gw_id   << " turns to be Active for ANA group " << state.ana_grp_id << dendl;
                        state.active_state(state.ana_grp_id);
                        propose = true;
                    }
                }
            }
    }
}


void  NVMeofGwMap::set_failover_gw_for_ANA_group(const GW_ID_T &failed_gw_id, const GROUP_KEY& group_key, const GW_ID_T &gw_id,  ANA_GRP_ID_T ANA_groupid)
{
    GW_CREATED_T& gw_state = Created_gws[group_key][gw_id];
    gw_state.failover_peer[ANA_groupid] = failed_gw_id;
    dout(4) << "Set failower GW " << gw_id << " for ANA group " << (int)ANA_groupid << dendl;

    gw_state.active_state(ANA_groupid);
}

void  NVMeofGwMap::find_failback_gw(const GW_ID_T &gw_id, const GROUP_KEY& group_key,   bool &some_found)
{
    bool  found_some_gw   = false;
    bool  found_candidate = false;
    auto& gws_states = Created_gws[group_key];
    auto& gw_state = Created_gws[group_key][gw_id];

    for (auto& gw_state_it: gws_states) {
        auto& failback_gw_id = gw_state_it.first;
        auto& st = gw_state_it.second;
        if (st.sm_state[gw_state.ana_grp_id] == GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE) {
            ceph_assert(st.failover_peer[gw_state.ana_grp_id] == gw_id);

            dout(4)  << "Found Failback GW " << failback_gw_id << " that previously took over the ANAGRP " << gw_state.ana_grp_id << " of the available GW " << gw_id << dendl;
            st.sm_state[gw_state.ana_grp_id] = GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED;
            start_timer(failback_gw_id, group_key, gw_state.ana_grp_id, 2);// Add timestamp of start Failback preparation
            gw_state.sm_state[gw_state.ana_grp_id] = GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL;
            found_candidate = true;
            break;
        }
        else if(st.sm_state[gw_state.ana_grp_id] == GW_STATES_PER_AGROUP_E::GW_WAIT_FOVER_BLIST_CMPL) {
            ceph_assert(st.failover_peer[gw_state.ana_grp_id] == gw_id);
            dout(4) << "Found Failback GW " << failback_gw_id << " that Waits to took over the ANAGRP " << gw_state.ana_grp_id << " of the available GW " << gw_id << dendl;
            found_candidate = false;
            break;
        }
        else found_some_gw = true;
    }
    some_found =  found_candidate |found_some_gw;
}


// TODO When decision to change ANA state of group is prepared, need to consider that last seen FSM state is "approved" - means it was returned in beacon alone with map version
void  NVMeofGwMap::find_failover_candidate(const GW_ID_T &gw_id, const GROUP_KEY& group_key, ANA_GRP_ID_T grpid, bool &propose_pending)
{
    // dout(4) <<__func__<< " process GW down " << gw_id << dendl;
    #define ILLEGAL_GW_ID " "
    #define MIN_NUM_ANA_GROUPS 0xFFF
    int min_num_ana_groups_in_gw = 0;
    int current_ana_groups_in_gw = 0;
    GW_ID_T min_loaded_gw_id = ILLEGAL_GW_ID;

    auto& gws_states = Created_gws[group_key];

    auto gw_state = gws_states.find(gw_id);
    ceph_assert(gw_state != gws_states.end());

    // this GW may handle several ANA groups and  for each of them need to found the candidate GW
    if (gw_state->second.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE || gw_state->second.ana_grp_id == grpid) {

        for (auto& found_gw_state: gws_states) { // for all the gateways of the subsystem
            auto st = found_gw_state.second;
            if(st.sm_state[grpid] ==  GW_STATES_PER_AGROUP_E::GW_WAIT_FOVER_BLIST_CMPL){   // some GW already started failover on this group
               dout(4) << "Failover already started for the group " << grpid <<  " by GW " << found_gw_state.first << dendl;
               gw_state->second.standby_state(grpid);
               return ;
            }
        }

        // Find a GW that takes over the ANA group(s)
        min_num_ana_groups_in_gw = MIN_NUM_ANA_GROUPS;
        min_loaded_gw_id = ILLEGAL_GW_ID;
        for (auto& found_gw_state: gws_states) { // for all the gateways of the subsystem
            auto st = found_gw_state.second;
            if (st.availability == GW_AVAILABILITY_E::GW_AVAILABLE) {
                current_ana_groups_in_gw = 0;
                for (int j = 0; j < MAX_SUPPORTED_ANA_GROUPS; j++) {
                    if (st.sm_state[j] == GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL || st.sm_state[j] == GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED
                                                                                          || st.sm_state[j] == GW_STATES_PER_AGROUP_E::GW_WAIT_FOVER_BLIST_CMPL){
                        current_ana_groups_in_gw = 0xFFFF;
                        break; // dont take into account   GWs in the transitive state
                    }
                    else if (st.sm_state[j] == GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE)
                        //dout(4) << " process GW down " << current_ana_groups_in_gw << dendl;
                        current_ana_groups_in_gw++; // how many ANA groups are handled by this GW
                    }

                    if (min_num_ana_groups_in_gw > current_ana_groups_in_gw) {
                        min_num_ana_groups_in_gw = current_ana_groups_in_gw;
                        min_loaded_gw_id = found_gw_state.first;
                        dout(4) << "choose: gw-id  min_ana_groups " << min_loaded_gw_id << current_ana_groups_in_gw << " min " << min_num_ana_groups_in_gw << dendl;
                    }
                }
            }
            if (min_loaded_gw_id != ILLEGAL_GW_ID) {
                propose_pending = true;
                set_failover_gw_for_ANA_group(gw_id, group_key, min_loaded_gw_id, grpid);
            }
            else {
                if (gw_state->second.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE){// not found candidate but map changed.
                    propose_pending = true;
                    dout(4) << "gw down no candidate found " << dendl;
                }
            }
            gw_state->second.standby_state(grpid);
        }
}

void NVMeofGwMap::fsm_handle_gw_alive (const GW_ID_T &gw_id, const GROUP_KEY& group_key,  GW_CREATED_T & gw_state, GW_STATES_PER_AGROUP_E state, ANA_GRP_ID_T grpid, epoch_t& last_osd_epoch, bool &map_modified)
{

}

 void NVMeofGwMap::fsm_handle_gw_down(const GW_ID_T &gw_id, const GROUP_KEY& group_key,   GW_STATES_PER_AGROUP_E state, ANA_GRP_ID_T grpid,  bool &map_modified)
 {
    switch (state)
    {
        case GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE:
        case GW_STATES_PER_AGROUP_E::GW_IDLE_STATE:
            // nothing to do
            break;

        case GW_STATES_PER_AGROUP_E::GW_WAIT_FOVER_BLIST_CMPL:
        {
            cancel_timer(gw_id, group_key, grpid);
        }break;

        case GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED:
            cancel_timer(gw_id, group_key,  grpid);

            for (auto& gw_st: Created_gws[group_key]) {
                auto& st = gw_st.second;
                if (st.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL) { // found GW   that was intended for  Failback for this ana grp
                    dout(4) << "Warning: Outgoing Failback when GW is down back - to rollback it"  <<" GW "  <<gw_id << "for ANA Group " << grpid << dendl;
                    st.sm_state[grpid] = GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE;
                    map_modified = true;
                    break;
                }
            }
            break;

        case GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL:
            // nothing to do - let failback timer expire
            break;

        case GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE:
        {
            find_failover_candidate( gw_id, group_key, grpid, map_modified);
        }
        break;

        default:{
            ceph_assert(false);
        }

    }
 }


void NVMeofGwMap::fsm_handle_gw_delete (const GW_ID_T &gw_id, const GROUP_KEY& group_key,
     GW_STATES_PER_AGROUP_E state , ANA_GRP_ID_T grpid, bool &map_modified) {
    switch (state)
    {
        case GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE:
        case GW_STATES_PER_AGROUP_E::GW_IDLE_STATE:
        case GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL:
        {
            GW_CREATED_T& gw_state = Created_gws[group_key][gw_id];

            if (grpid == gw_state.ana_grp_id) {// Try to find GW that temporary owns my group - if found, this GW should pass to standby for  this group
                auto& gateway_states = Created_gws[group_key];
                for (auto& gs: gateway_states) {
                    if (gs.second.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE  || gs.second.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED){
                        gs.second.standby_state(grpid);
                        map_modified = true;
                        if (gs.second.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED)
                            cancel_timer(gs.first, group_key, grpid);
                        break;
                    }
                }
            }
        }
        break;

        case GW_STATES_PER_AGROUP_E::GW_WAIT_FOVER_BLIST_CMPL:
        {
            cancel_timer(gw_id, group_key, grpid);
        }
        break;

        case GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED:
        {
            cancel_timer(gw_id, group_key, grpid);
            for (auto& nqn_gws_state: Created_gws[group_key]) {
                auto& st = nqn_gws_state.second;

                if (st.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL) { // found GW   that was intended for  Failback for this ana grp
                    dout(4) << "Warning: Outgoing Failback when GW is deleted - to rollback it" << " GW " << gw_id << "for ANA Group " << grpid << dendl;
                    st.standby_state(grpid);
                    map_modified = true;
                    break;
                }
            }
        }
        break;

        case GW_STATES_PER_AGROUP_E::GW_ACTIVE_STATE:
        {
            GW_CREATED_T& gw_state = Created_gws[group_key][gw_id];
            map_modified = true;
            gw_state.standby_state(grpid);
        }
        break;

        default: {
            ceph_assert(false);
        }
    }
}

void NVMeofGwMap::fsm_handle_to_expired(const GW_ID_T &gw_id, const GROUP_KEY& group_key, ANA_GRP_ID_T grpid,  bool &map_modified)
{
    auto& fbp_gw_state = Created_gws[group_key][gw_id];// GW in Fail-back preparation state fbp

    if (fbp_gw_state.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_WAIT_FAILBACK_PREPARED) {

        cancel_timer(gw_id, group_key, grpid);
        //TODO  handle the case when epoch was not changed - re-arm the timer
        for (auto& gw_state: Created_gws[group_key]) {
            auto& st = gw_state.second;
            if (st.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_OWNER_WAIT_FBACK_BLIST_CMPL &&
                    st.availability == GW_AVAILABILITY_E::GW_AVAILABLE) //&&
                    //st.blocklist_data[grpid].epoch_changed ==true)
            {                             //TODO process the scenario when timer expired but epoch_changed = false
                                          // propose to make 2 retries with arming of timer if not helps - to assert
                dout(4)  << "Expired Failback timer from GW " << gw_id << " ANA groupId "<< grpid << "osd epoch "  << st.blocklist_data[grpid].osd_epoch  << dendl;

                fbp_gw_state.standby_state(grpid);
                st.active_state(grpid);
                dout(4) << "Failback from GW " << gw_id << " to " << gw_state.first << dendl;
                map_modified = true;
                break;
            }
            else if (st.ana_grp_id == grpid){
                if(st.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE  &&  st.availability == GW_AVAILABILITY_E::GW_AVAILABLE) {
                    st.active_state(grpid);// GW failed and started during the persistency interval
                    dout(4)  << "Failback unsuccessfull. GW: " << gw_state.first << "becomes Active for the ana group " << grpid  << dendl;
                }
                fbp_gw_state.standby_state(grpid);
                dout(4) << "Failback unsuccessfull GW: " << gw_id << "becomes standby for the ana group " << grpid  << dendl;
                map_modified = true;
                break;
            }
        }
    }
    else if(fbp_gw_state.sm_state[grpid] == GW_STATES_PER_AGROUP_E::GW_WAIT_FOVER_BLIST_CMPL){
        dout(4) << " Expired GW_WAIT_FAILOVER_PREPARED timer from GW " << gw_id << " ANA groupId: "<< grpid << dendl;
        fbp_gw_state.blocklist_data[grpid].epoch_changed = true;
        fbp_gw_state.sm_state[grpid] =  GW_STATES_PER_AGROUP_E::GW_STANDBY_STATE;
        map_modified = true;
        //ceph_assert(false);
    }
}

GW_CREATED_T& NVMeofGwMap::find_already_created_gw(const GW_ID_T &gw_id, const GROUP_KEY& group_key)
{
    auto& group_gws = Created_gws[group_key];
    auto  gw_it = group_gws.find(gw_id);
    ceph_assert(gw_it != group_gws.end());//should not happen
    return gw_it->second;
}

int NVMeofGwMap::blocklist_gw(const GW_ID_T &gw_id, const GROUP_KEY& group_key, ANA_GRP_ID_T grpid, epoch_t &epoch)
{
    return 1;
}

void NVMeofGwMap::start_timer(const GW_ID_T &gw_id, const GROUP_KEY& group_key, ANA_GRP_ID_T anagrpid, uint8_t value) {
    Gmetadata[group_key][gw_id].data[anagrpid].anagrp_sm_tstamps = 0;
    Gmetadata[group_key][gw_id].data[anagrpid].timer_value = value;
}

int  NVMeofGwMap::get_timer(const GW_ID_T &gw_id, const GROUP_KEY& group_key, ANA_GRP_ID_T anagrpid) {
    auto timer = Gmetadata[group_key][gw_id].data[anagrpid].anagrp_sm_tstamps;
    ceph_assert(timer != INVALID_GW_TIMER);
    return timer;
}

void NVMeofGwMap::cancel_timer(const GW_ID_T &gw_id, const GROUP_KEY& group_key, ANA_GRP_ID_T anagrpid) {
    Gmetadata[group_key][gw_id].data[anagrpid].anagrp_sm_tstamps = INVALID_GW_TIMER;
}
