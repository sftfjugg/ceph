// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */



#include "include/types.h"
#include "common/Clock.h"

#include "msg/Messenger.h"

#include "osd/OSDMap.h"
#include "osdc/Objecter.h"
#include "osdc/Filer.h"

#include "MDSMap.h"

#include "MDS.h"
#include "Server.h"
#include "Locker.h"
#include "MDCache.h"
#include "MDLog.h"
#include "MDBalancer.h"
#include "IdAllocator.h"
#include "Migrator.h"
//#include "Renamer.h"

#include "AnchorTable.h"
#include "AnchorClient.h"

#include "common/Logger.h"
#include "common/LogType.h"

#include "common/Timer.h"

#include "events/EClientMap.h"

#include "messages/MMDSMap.h"
#include "messages/MMDSBeacon.h"

#include "messages/MPing.h"
#include "messages/MPingAck.h"
#include "messages/MGenericMessage.h"

#include "messages/MOSDMap.h"
#include "messages/MOSDGetMap.h"

#include "messages/MClientRequest.h"
#include "messages/MClientRequestForward.h"


LogType mds_logtype, mds_cache_logtype;

#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) cout << g_clock.now() << " mds" << whoami << " "
#define  derr(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) cout << g_clock.now() << " mds" << whoami << " "





// cons/des
MDS::MDS(int whoami, Messenger *m, MonMap *mm) : timer(mds_lock) {
  this->whoami = whoami;

  monmap = mm;
  messenger = m;

  mdsmap = new MDSMap;
  osdmap = new OSDMap;

  objecter = new Objecter(messenger, monmap, osdmap);
  filer = new Filer(objecter);

  mdcache = new MDCache(this);
  mdlog = new MDLog(this);
  balancer = new MDBalancer(this);

  anchorclient = new AnchorClient(this);
  idalloc = new IdAllocator(this);

  anchortable = new AnchorTable(this);

  server = new Server(this);
  locker = new Locker(this, mdcache);


  // clients
  last_client_mdsmap_bcast = 0;
  
  // beacon
  beacon_last_seq = 0;
  beacon_sender = 0;
  beacon_killer = 0;

  // tick
  tick_event = 0;

  req_rate = 0;

  want_state = state = MDSMap::STATE_DNE;

  logger = logger2 = 0;

  // i'm ready!
  messenger->set_dispatcher(this);
}

MDS::~MDS() {
  if (mdcache) { delete mdcache; mdcache = NULL; }
  if (mdlog) { delete mdlog; mdlog = NULL; }
  if (balancer) { delete balancer; balancer = NULL; }
  if (idalloc) { delete idalloc; idalloc = NULL; }
  if (anchortable) { delete anchortable; anchortable = NULL; }
  if (anchorclient) { delete anchorclient; anchorclient = NULL; }
  if (osdmap) { delete osdmap; osdmap = 0; }
  if (mdsmap) { delete mdsmap; mdsmap = 0; }

  if (server) { delete server; server = 0; }
  if (locker) { delete locker; locker = 0; }

  if (filer) { delete filer; filer = 0; }
  if (objecter) { delete objecter; objecter = 0; }
  if (messenger) { delete messenger; messenger = NULL; }

  if (logger) { delete logger; logger = 0; }
  if (logger2) { delete logger2; logger2 = 0; }

}


void MDS::reopen_logger()
{
  // flush+close old log
  if (logger) {
    logger->flush(true);
    delete logger;
  }
  if (logger2) {
    logger2->flush(true);
    delete logger2;
  }


  // log
  string name;
  name = "mds";
  int w = whoami;
  if (w >= 1000) name += ('0' + ((w/1000)%10));
  if (w >= 100) name += ('0' + ((w/100)%10));
  if (w >= 10) name += ('0' + ((w/10)%10));
  name += ('0' + ((w/1)%10));

  logger = new Logger(name, (LogType*)&mds_logtype);

  mds_logtype.add_inc("req");
  mds_logtype.add_inc("reply");
  mds_logtype.add_inc("fw");
  mds_logtype.add_inc("cfw");

  mds_logtype.add_set("l");
  mds_logtype.add_set("q");
  mds_logtype.add_set("popanyd");
  mds_logtype.add_set("popnest");

  mds_logtype.add_inc("lih");
  mds_logtype.add_inc("lif");

  mds_logtype.add_set("c");
  mds_logtype.add_set("ctop");
  mds_logtype.add_set("cbot");
  mds_logtype.add_set("cptail");  
  mds_logtype.add_set("cpin");
  mds_logtype.add_inc("cex");
  mds_logtype.add_inc("dis");
  mds_logtype.add_inc("cmiss");

  mds_logtype.add_set("buf");
  mds_logtype.add_inc("cdir");
  mds_logtype.add_inc("fdir");

  mds_logtype.add_inc("iex");
  mds_logtype.add_inc("iim");
  mds_logtype.add_inc("ex");
  mds_logtype.add_inc("im");
  mds_logtype.add_inc("imex");  
  mds_logtype.add_set("nex");
  mds_logtype.add_set("nim");

  
  char n[80];
  sprintf(n, "mds%d.cache", whoami);
  logger2 = new Logger(n, (LogType*)&mds_cache_logtype);
}

void MDS::send_message_mds(Message *m, int mds, int port, int fromport)
{
  // send mdsmap first?
  if (peer_mdsmap_epoch[mds] < mdsmap->get_epoch()) {
    messenger->send_message(new MMDSMap(mdsmap), 
			    mdsmap->get_inst(mds));
    peer_mdsmap_epoch[mds] = mdsmap->get_epoch();
  }

  // send message
  if (port && !fromport) 
    fromport = port;
  messenger->send_message(m, mdsmap->get_inst(mds), port, fromport);
}

void MDS::forward_message_mds(Message *req, int mds, int port)
{
  // client request?
  if (req->get_type() == MSG_CLIENT_REQUEST) {
    MClientRequest *creq = (MClientRequest*)req;
    creq->inc_num_fwd();    // inc forward counter

    // tell the client where it should go
    messenger->send_message(new MClientRequestForward(creq->get_tid(), mds, creq->get_num_fwd()),
			    creq->get_client_inst());
    
    if (!creq->is_idempotent()) {
      delete req;
      return;  // don't actually forward if non-idempotent
    }
  }
  
  // forward
  send_message_mds(req, mds, port);
}


class C_MDS_Tick : public Context {
  MDS *mds;
public:
  C_MDS_Tick(MDS *m) : mds(m) {}
  void finish(int r) {
    mds->tick();
  }
};



int MDS::init(bool standby)
{
  mds_lock.Lock();
  
  want_state = MDSMap::STATE_BOOT;
  
  // starting beacon.  this will induce an MDSMap from the monitor
  beacon_start();
  
  // schedule tick
  reset_tick();

  // init logger
  reopen_logger();

  mds_lock.Unlock();
  return 0;
}

void MDS::reset_tick()
{
  // cancel old
  if (tick_event) timer.cancel_event(tick_event);

  // schedule
  tick_event = new C_MDS_Tick(this);
  timer.add_event_after(g_conf.mon_tick_interval, tick_event);
}

void MDS::tick()
{
  // reschedule
  reset_tick();

  // log
  mds_load_t load = balancer->get_load();
  
  if (logger) {
    req_rate = logger->get("req");
    
    logger->set("l", (int)load.mds_load());
    logger->set("q", messenger->get_dispatch_queue_len());
    logger->set("buf", buffer_total_alloc);
    
    mdcache->log_stat(logger);
  }
  
  // booted?
  if (is_active()) {
    
    // balancer
    balancer->tick();
    
    // HACK to test hashing stuff
    if (false) {
      /*
      static map<int,int> didhash;
      if (elapsed.sec() > 15 && !didhash[whoami]) {
	CInode *in = mdcache->get_inode(100000010);
	if (in && in->dir) {
	  if (in->dir->is_auth()) 
	    mdcache->migrator->hash_dir(in->dir);
	  didhash[whoami] = 1;
	}
      }
      if (0 && elapsed.sec() > 25 && didhash[whoami] == 1) {
	CInode *in = mdcache->get_inode(100000010);
	if (in && in->dir) {
	  if (in->dir->is_auth() && in->dir->is_hashed())
	    mdcache->migrator->unhash_dir(in->dir);
	  didhash[whoami] = 2;
	}
      }
      */
    }
  }
}




// -----------------------
// beacons

void MDS::beacon_start()
{
  beacon_send();         // send first beacon
  
  //reset_beacon_killer(); // schedule killer
}
  

class C_MDS_BeaconSender : public Context {
  MDS *mds;
public:
  C_MDS_BeaconSender(MDS *m) : mds(m) {}
  void finish(int r) {
    mds->beacon_send();
  }
};

void MDS::beacon_send()
{
  ++beacon_last_seq;
  dout(10) << "beacon_send " << MDSMap::get_state_name(want_state)
	   << " seq " << beacon_last_seq
	   << " (currently " << MDSMap::get_state_name(state) << ")"
	   << endl;

  beacon_seq_stamp[beacon_last_seq] = g_clock.now();
  
  int mon = monmap->pick_mon();
  messenger->send_message(new MMDSBeacon(messenger->get_myinst(), want_state, beacon_last_seq),
			  monmap->get_inst(mon));

  // schedule next sender
  if (beacon_sender) timer.cancel_event(beacon_sender);
  beacon_sender = new C_MDS_BeaconSender(this);
  timer.add_event_after(g_conf.mds_beacon_interval, beacon_sender);
}

void MDS::handle_mds_beacon(MMDSBeacon *m)
{
  dout(10) << "handle_mds_beacon " << MDSMap::get_state_name(m->get_state())
	   << " seq " << m->get_seq() << endl;
  version_t seq = m->get_seq();
  
  // update lab
  if (beacon_seq_stamp.count(seq)) {
    assert(beacon_seq_stamp[seq] > beacon_last_acked_stamp);
    beacon_last_acked_stamp = beacon_seq_stamp[seq];
    
    // clean up seq_stamp map
    while (!beacon_seq_stamp.empty() &&
	   beacon_seq_stamp.begin()->first <= seq)
      beacon_seq_stamp.erase(beacon_seq_stamp.begin());
    
    reset_beacon_killer();
  }

  delete m;
}

class C_MDS_BeaconKiller : public Context {
  MDS *mds;
  utime_t lab;
public:
  C_MDS_BeaconKiller(MDS *m, utime_t l) : mds(m), lab(l) {}
  void finish(int r) {
    mds->beacon_kill(lab);
  }
};

void MDS::reset_beacon_killer()
{
  utime_t when = beacon_last_acked_stamp;
  when += g_conf.mds_beacon_grace;
  
  dout(15) << "reset_beacon_killer last_acked_stamp at " << beacon_last_acked_stamp
	   << ", will die at " << when << endl;
  
  if (beacon_killer) timer.cancel_event(beacon_killer);

  beacon_killer = new C_MDS_BeaconKiller(this, beacon_last_acked_stamp);
  timer.add_event_at(when, beacon_killer);
}

void MDS::beacon_kill(utime_t lab)
{
  if (lab == beacon_last_acked_stamp) {
    dout(0) << "beacon_kill last_acked_stamp " << lab 
	    << ", killing myself."
	    << endl;
    exit(0);
  } else {
    dout(20) << "beacon_kill last_acked_stamp " << beacon_last_acked_stamp 
	     << " != my " << lab 
	     << ", doing nothing."
	     << endl;
  }
}



void MDS::handle_mds_map(MMDSMap *m)
{
  version_t hadepoch = mdsmap->get_epoch();
  version_t epoch = m->get_epoch();
  dout(5) << "handle_mds_map epoch " << epoch << " from " << m->get_source() << endl;

  // note source's map version
  if (m->get_source().is_mds() && 
      peer_mdsmap_epoch[m->get_source().num()] < epoch) {
    dout(15) << " peer " << m->get_source()
	     << " has mdsmap epoch >= " << epoch
	     << endl;
    peer_mdsmap_epoch[m->get_source().num()] = epoch;
  }

  // is it new?
  if (epoch <= mdsmap->get_epoch()) {
    dout(5) << " old map epoch " << epoch << " <= " << mdsmap->get_epoch() 
	    << ", discarding" << endl;
    delete m;
    return;
  }

  // note some old state
  int oldwhoami = whoami;
  int oldstate = state;
  set<int> oldresolve;
  mdsmap->get_mds_set(oldresolve, MDSMap::STATE_RESOLVE);
  bool wasrejoining = mdsmap->is_rejoining();
  set<int> oldfailed;
  mdsmap->get_mds_set(oldfailed, MDSMap::STATE_FAILED);
  set<int> oldactive;
  mdsmap->get_mds_set(oldactive, MDSMap::STATE_ACTIVE);
  set<int> oldcreating;
  mdsmap->get_mds_set(oldcreating, MDSMap::STATE_CREATING);
  set<int> oldout;
  mdsmap->get_mds_set(oldout, MDSMap::STATE_OUT);

  // decode and process
  mdsmap->decode(m->get_encoded());
  
  // see who i am
  whoami = mdsmap->get_addr_rank(messenger->get_myaddr());
  if (oldwhoami != whoami) {
    // update messenger.
    messenger->reset_myname(MSG_ADDR_MDS(whoami));

    reopen_logger();
    dout(1) << "handle_mds_map i am now mds" << whoami
	    << " incarnation " << mdsmap->get_inc(whoami)
	    << endl;

    // do i need an osdmap?
    if (oldwhoami < 0) {
      // we need an osdmap too.
      int mon = monmap->pick_mon();
      messenger->send_message(new MOSDGetMap(0),
			      monmap->get_inst(mon));
    }
  }

  // tell objecter my incarnation
  if (objecter->get_client_incarnation() < 0 &&
      mdsmap->have_inst(whoami)) {
    assert(mdsmap->get_inc(whoami) > 0);
    objecter->set_client_incarnation(mdsmap->get_inc(whoami));
  }

  // for debug
  if (g_conf.mds_dump_cache_on_map)
    mdcache->dump_cache();

  // update my state
  state = mdsmap->get_state(whoami);
  
  // did it change?
  if (oldstate != state) {
    if (state == want_state) {
      dout(1) << "handle_mds_map new state " << mdsmap->get_state_name(state) << endl;
    } else {
      dout(1) << "handle_mds_map new state " << mdsmap->get_state_name(state)
	      << ", although i wanted " << mdsmap->get_state_name(want_state)
	      << endl;
      want_state = state;
    }    

    // contemplate suicide
    if (mdsmap->get_inst(whoami) != messenger->get_myinst()) {
      dout(1) << "apparently i've been replaced by " << mdsmap->get_inst(whoami) << ", committing suicide." << endl;
      exit(-1);
    }
    if (mdsmap->is_down(whoami)) {
      dout(1) << "apparently i'm down. committing suicide." << endl;
      exit(-1);
    }

    // now active?
    if (is_active()) {
      // did i just recover?
      if (oldstate == MDSMap::STATE_REJOIN) {
	dout(1) << "successful recovery!" << endl;

	// kick anchortable (resent AGREEs)
	if (mdsmap->get_anchortable() == whoami) 
	  anchortable->finish_recovery();

	// kick anchorclient (resent COMMITs)
	anchorclient->finish_recovery();

	mdcache->start_recovered_purges();
	
	// tell connected clients
	bcast_mds_map();  
      }

      dout(1) << "now active" << endl;
      finish_contexts(waitfor_active);  // kick waiters
    }

    else if (is_reconnect()) {
      server->reconnect_clients();
    }

    else if (is_replay()) {
      // initialize gather sets
      set<int> rs;
      mdsmap->get_recovery_mds_set(rs);
      rs.erase(whoami);
      dout(1) << "now replay.  my recovery peers are " << rs << endl;
      mdcache->set_recovery_set(rs);
    }
    
    // now stopping?
    else if (is_stopping()) {
      assert(oldstate == MDSMap::STATE_ACTIVE);
      dout(1) << "now stopping" << endl;
      
      // start cache shutdown
      mdcache->shutdown_start();

      // terminate client sessions
      server->terminate_sessions();
      
      // flush log
      mdlog->set_max_events(0);
      mdlog->trim(NULL);
    }

    // now standby?
    else if (is_stopped()) {
      assert(oldstate == MDSMap::STATE_STOPPING);
      dout(1) << "now stopped, sending down:out and exiting" << endl;
      shutdown_final();
    }
  }
  
  
  // RESOLVE
  // am i newly resolving?
  if (is_resolve() && oldstate == MDSMap::STATE_REPLAY) {
    // send to all resolve, active, stopping
    dout(10) << "i am newly resolving, sharing import map" << endl;
    set<int> who;
    mdsmap->get_mds_set(who, MDSMap::STATE_RESOLVE);
    mdsmap->get_mds_set(who, MDSMap::STATE_ACTIVE);
    mdsmap->get_mds_set(who, MDSMap::STATE_STOPPING);
    mdsmap->get_mds_set(who, MDSMap::STATE_REJOIN);     // hrm. FIXME.
    for (set<int>::iterator p = who.begin(); p != who.end(); ++p) {
      if (*p == whoami) continue;
      mdcache->send_import_map(*p);  // now.
    }
  }
  // is someone else newly resolving?
  else if (is_resolve() || is_rejoin() || is_active() || is_stopping()) {
    set<int> resolve;
    mdsmap->get_mds_set(resolve, MDSMap::STATE_RESOLVE);
    if (oldresolve != resolve) {
      dout(10) << "resolve set is " << resolve << ", was " << oldresolve << endl;
      for (set<int>::iterator p = resolve.begin(); p != resolve.end(); ++p) {
	if (*p == whoami) continue;
	if (oldresolve.count(*p)) continue;
	mdcache->send_import_map(*p);  // now or later.
      }
    }
  }
  
  // REJOIN
  // is everybody finally rejoining?
  if (is_rejoin() || is_active() || is_stopping()) {
    // did we start?
    if (!wasrejoining && mdsmap->is_rejoining()) {
      mdcache->send_cache_rejoins();
    }
    // did we finish?
    if (wasrejoining && !mdsmap->is_rejoining()) {
      mdcache->dump_cache();
    }
  }

  // did someone go active?
  if (is_active() || is_stopping()) {
    set<int> active;
    mdsmap->get_mds_set(active, MDSMap::STATE_ACTIVE);
    for (set<int>::iterator p = active.begin(); p != active.end(); ++p) {
      if (*p == whoami) continue;         // not me
      if (oldactive.count(*p)) continue;  // newly so?
      mdcache->handle_mds_recovery(*p);
      if (anchortable)
	anchortable->handle_mds_recovery(*p);
      anchorclient->handle_mds_recovery(*p);
    }
  }

  // did anyone go down?
  if (is_active() || is_stopping()) {
    set<int> failed;
    mdsmap->get_mds_set(failed, MDSMap::STATE_FAILED);
    for (set<int>::iterator p = failed.begin(); p != failed.end(); ++p) {
      // newly so?
      if (oldfailed.count(*p)) continue;      

      mdcache->handle_mds_failure(*p);
    }
  }

  // inst set changed?
  /*
  if (state >= MDSMap::STATE_ACTIVE &&   // only if i'm active+.  otherwise they'll get map during reconnect.
      mdsmap->get_same_inst_since() > last_client_mdsmap_bcast) {
    bcast_mds_map();
  }
  */

  // just got mdsmap+osdmap?
  if (hadepoch == 0 && 
      mdsmap->get_epoch() > 0 &&
      osdmap->get_epoch() > 0)
    boot();

  delete m;
}

void MDS::bcast_mds_map()
{
  dout(7) << "bcast_mds_map " << mdsmap->get_epoch() << endl;

  // share the map with mounted clients
  for (set<int>::const_iterator p = clientmap.get_session_set().begin();
       p != clientmap.get_session_set().end();
       ++p) {
    messenger->send_message(new MMDSMap(mdsmap),
			    clientmap.get_inst(*p));
  }
  last_client_mdsmap_bcast = mdsmap->get_epoch();
}


void MDS::handle_osd_map(MOSDMap *m)
{
  version_t hadepoch = osdmap->get_epoch();
  dout(10) << "handle_osd_map had " << hadepoch << endl;
  
  // process
  objecter->handle_osd_map(m);

  // just got mdsmap+osdmap?
  if (hadepoch == 0 && 
      osdmap->get_epoch() > 0 &&
      mdsmap->get_epoch() > 0) 
    boot();
}


void MDS::boot()
{   
  if (is_creating()) 
    boot_create();    // new tables, journal
  else if (is_starting())
    boot_start();     // old tables, empty journal
  else if (is_replay()) 
    boot_replay();    // replay, join
  else 
    assert(0);
}


class C_MDS_BootFinish : public Context {
  MDS *mds;
public:
  C_MDS_BootFinish(MDS *m) : mds(m) {}
  void finish(int r) { mds->boot_finish(); }
};

void MDS::boot_create()
{
  dout(3) << "boot_create" << endl;

  C_Gather *fin = new C_Gather(new C_MDS_BootFinish(this));

  if (whoami == 0) {
    dout(3) << "boot_create since i am also mds0, creating root inode and dir" << endl;

    // create root inode.
    mdcache->open_root(0);
    CInode *root = mdcache->get_root();
    assert(root);
    
    // force empty root dir
    CDir *dir = root->get_dirfrag(frag_t());
    dir->mark_complete();
    dir->mark_dirty(dir->pre_dirty());
    
    // save it
    dir->commit(0, fin->new_sub());
  }

  // create my stray dir
  {
    dout(10) << "boot_create creating local stray dir" << endl;
    mdcache->open_local_stray();
    CInode *stray = mdcache->get_stray();
    CDir *dir = stray->get_dirfrag(frag_t());
    dir->mark_complete();
    dir->mark_dirty(dir->pre_dirty());
    dir->commit(0, fin->new_sub());
  }

  // start with a fresh journal
  dout(10) << "boot_create creating fresh journal" << endl;
  mdlog->reset();
  mdlog->write_head(fin->new_sub());
  
  // write our first importmap
  mdcache->log_import_map(fin->new_sub());

  // fixme: fake out idalloc (reset, pretend loaded)
  dout(10) << "boot_create creating fresh idalloc table" << endl;
  idalloc->reset();
  idalloc->save(fin->new_sub());
  
  // fixme: fake out anchortable
  if (mdsmap->get_anchortable() == whoami) {
    dout(10) << "boot_create creating fresh anchortable" << endl;
    anchortable->create_fresh();
    anchortable->save(fin->new_sub());
  }
}

void MDS::boot_start()
{
  dout(2) << "boot_start" << endl;
  
  C_Gather *fin = new C_Gather(new C_MDS_BootFinish(this));
  
  dout(2) << "boot_start opening idalloc" << endl;
  idalloc->load(fin->new_sub());
  
  if (mdsmap->get_anchortable() == whoami) {
    dout(2) << "boot_start opening anchor table" << endl;
    anchortable->load(fin->new_sub());
  } else {
    dout(2) << "boot_start i have no anchor table" << endl;
  }

  dout(2) << "boot_start opening mds log" << endl;
  mdlog->open(fin->new_sub());

  if (mdsmap->get_root() == whoami) {
    dout(2) << "boot_start opening root directory" << endl;
    mdcache->open_root(fin->new_sub());
  }

  dout(2) << "boot_start opening local stray directory" << endl;
  mdcache->open_local_stray();
}

void MDS::boot_finish()
{
  dout(3) << "boot_finish" << endl;

  if (is_starting()) {
    // make sure mdslog is empty
    assert(mdlog->get_read_pos() == mdlog->get_write_pos());
  }

  set_want_state(MDSMap::STATE_ACTIVE);
}


class C_MDS_BootRecover : public Context {
  MDS *mds;
  int nextstep;
public:
  C_MDS_BootRecover(MDS *m, int n) : mds(m), nextstep(n) {}
  void finish(int r) { mds->boot_replay(nextstep); }
};

void MDS::boot_replay(int step)
{
  switch (step) {
  case 0:
    step = 1;  // fall-thru.

  case 1:
    dout(2) << "boot_replay " << step << ": opening idalloc" << endl;
    idalloc->load(new C_MDS_BootRecover(this, 2));
    break;

  case 2:
    if (mdsmap->get_anchortable() == whoami) {
      dout(2) << "boot_replay " << step << ": opening anchor table" << endl;
      anchortable->load(new C_MDS_BootRecover(this, 3));
      break;
    }
    dout(2) << "boot_replay " << step << ": i have no anchor table" << endl;
    step++; // fall-thru

  case 3:
    dout(2) << "boot_replay " << step << ": opening mds log" << endl;
    mdlog->open(new C_MDS_BootRecover(this, 4));
    break;
    
  case 4:
    dout(2) << "boot_replay " << step << ": replaying mds log" << endl;
    mdlog->replay(new C_MDS_BootRecover(this, 5));
    break;

  case 5:
    // done with replay!
    if (mdsmap->get_num_in_mds() == 1) { // me
      dout(2) << "boot_replay " << step << ": i am alone, moving to state reconnect" << endl;      
      set_want_state(MDSMap::STATE_RECONNECT);
    } else {
      dout(2) << "boot_replay " << step << ": i am not alone, moving to state resolve" << endl;
      set_want_state(MDSMap::STATE_RESOLVE);
    }
    break;

  }
}


void MDS::set_want_state(int s)
{
  dout(3) << "set_want_state " << MDSMap::get_state_name(s) << endl;
  want_state = s;
  beacon_send();
}




int MDS::shutdown_start()
{
  dout(1) << "shutdown_start" << endl;
  derr(0) << "mds shutdown start" << endl;

  // tell everyone to stop.
  set<int> active;
  mdsmap->get_active_mds_set(active);
  for (set<int>::iterator p = active.begin();
       p != active.end();
       p++) {
    if (mdsmap->is_up(*p)) {
      dout(1) << "sending MShutdownStart to mds" << *p << endl;
      send_message_mds(new MGenericMessage(MSG_MDS_SHUTDOWNSTART),
		       *p, MDS_PORT_MAIN);
    }
  }

  // go
  set_want_state(MDSMap::STATE_STOPPING);
  return 0;
}


void MDS::handle_shutdown_start(Message *m)
{
  dout(1) << " handle_shutdown_start" << endl;

  // set flag
  set_want_state(MDSMap::STATE_STOPPING);

  delete m;
}



int MDS::shutdown_final()
{
  dout(1) << "shutdown_final" << endl;

  // flush loggers
  if (logger) logger->flush(true);
  if (logger2) logger2->flush(true);
  mdlog->flush_logger();
  
  // send final down:out beacon (it doesn't matter if this arrives)
  set_want_state(MDSMap::STATE_OUT);

  // stop timers
  if (beacon_killer) {
    timer.cancel_event(beacon_killer);
    beacon_killer = 0;
  }
  if (beacon_sender) {
    timer.cancel_event(beacon_sender);
    beacon_sender = 0;
  }
  if (tick_event) {
    timer.cancel_event(tick_event);
    tick_event = 0;
  }
  timer.cancel_all();
  timer.join();
  
  // shut down cache
  mdcache->shutdown();
  
  // shut down messenger
  messenger->shutdown();
  
  return 0;
}





void MDS::dispatch(Message *m)
{
  mds_lock.Lock();
  my_dispatch(m);
  mds_lock.Unlock();
}



void MDS::my_dispatch(Message *m)
{
  // from bad mds?
  if (m->get_source().is_mds()) {
    int from = m->get_source().num();
    if (!mdsmap->have_inst(from) ||
	mdsmap->get_inst(from) != m->get_source_inst() ||
	mdsmap->is_down(from)) {
      // bogus mds?
      if (m->get_type() != MSG_MDS_MAP) {
	dout(5) << "got " << *m << " from down/old/bad/imposter mds " << m->get_source()
		<< ", dropping" << endl;
	delete m;
	return;
      } else {
	dout(5) << "got " << *m << " from old/bad/imposter mds " << m->get_source()
		<< ", but it's an mdsmap, looking at it" << endl;
      }
    }
  }


  switch (m->get_dest_port()) {
    
  case MDS_PORT_ANCHORTABLE:
    anchortable->dispatch(m);
    break;
  case MDS_PORT_ANCHORCLIENT:
    anchorclient->dispatch(m);
    break;
    
  case MDS_PORT_CACHE:
    mdcache->dispatch(m);
    break;
  case MDS_PORT_LOCKER:
    locker->dispatch(m);
    break;

  case MDS_PORT_MIGRATOR:
    mdcache->migrator->dispatch(m);
    break;
  case MDS_PORT_RENAMER:
    //mdcache->renamer->dispatch(m);
    break;

  case MDS_PORT_BALANCER:
    balancer->proc_message(m);
    break;
    
  case MDS_PORT_MAIN:
    proc_message(m);
    break;

  case MDS_PORT_SERVER:
    server->dispatch(m);
    break;

  default:
    dout(1) << "MDS dispatch unknown message port" << m->get_dest_port() << endl;
    assert(0);
  }
  
  // finish any triggered contexts
  if (finished_queue.size()) {
    dout(7) << "mds has " << finished_queue.size() << " queued contexts" << endl;
    dout(10) << finished_queue << endl;
    list<Context*> ls;
    ls.splice(ls.begin(), finished_queue);
    assert(finished_queue.empty());
    finish_contexts(ls);
  }


  // HACK FOR NOW
  if (is_active()) {
    // flush log to disk after every op.  for now.
    mdlog->flush();

    // trim cache
    mdcache->trim();
  }

  
  // hack: thrash exports
  for (int i=0; i<g_conf.mds_thrash_exports; i++) {
    set<int> s;
    if (!is_active()) break;
    mdsmap->get_mds_set(s, MDSMap::STATE_ACTIVE);
    if (s.size() < 2 || mdcache->get_num_inodes() < 10) 
      break;  // need peers for this to work.

    dout(7) << "mds thrashing exports pass " << (i+1) << "/" << g_conf.mds_thrash_exports << endl;
    
    // pick a random dir inode
    CInode *in = mdcache->hack_pick_random_inode();

    list<CDir*> ls;
    in->get_dirfrags(ls);
    if (ls.empty()) continue;                // must be an open dir.
    CDir *dir = ls.front();
    if (!dir->get_parent_dir()) continue;    // must be linked.
    if (!dir->is_auth()) continue;           // must be auth.

    int dest;
    do {
      int k = rand() % s.size();
      set<int>::iterator p = s.begin();
      while (k--) p++;
      dest = *p;
    } while (dest == whoami);
    mdcache->migrator->export_dir(dir,dest);
  }


  // hack: force hash root?
  /*
  if (false &&
      mdcache->get_root() &&
      mdcache->get_root()->dir &&
      !(mdcache->get_root()->dir->is_hashed() || 
        mdcache->get_root()->dir->is_hashing())) {
    dout(0) << "hashing root" << endl;
    mdcache->migrator->hash_dir(mdcache->get_root()->dir);
  }
  */



  // HACK to force export to test foreign renames
  if (false && whoami == 0) {
    /*
    static bool didit = false;
    
    // 7 to 1
    CInode *in = mdcache->get_inode(1001);
    if (in && in->is_dir() && !didit) {
      CDir *dir = in->get_or_open_dir(mdcache);
      if (dir->is_auth()) {
        dout(1) << "FORCING EXPORT" << endl;
        mdcache->migrator->export_dir(dir,1);
        didit = true;
      }
    }
    */
  }



  // shut down?
  if (is_stopping()) {
    if (mdcache->shutdown_pass()) {
      dout(7) << "shutdown_pass=true, finished w/ shutdown, moving to up:stopped" << endl;

      // tell monitor we shut down cleanly.
      set_want_state(MDSMap::STATE_STOPPED);
    }
  }

}


void MDS::proc_message(Message *m)
{
  switch (m->get_type()) {
    // OSD ===============
    /*
  case MSG_OSD_MKFS_ACK:
    handle_osd_mkfs_ack(m);
    return;
    */
  case MSG_OSD_OPREPLY:
    objecter->handle_osd_op_reply((class MOSDOpReply*)m);
    return;
  case MSG_OSD_MAP:
    handle_osd_map((MOSDMap*)m);
    return;


    // MDS
  case MSG_MDS_MAP:
    handle_mds_map((MMDSMap*)m);
    return;

  case MSG_MDS_BEACON:
    handle_mds_beacon((MMDSBeacon*)m);
    return;

  case MSG_MDS_SHUTDOWNSTART:    // mds0 -> mds1+
    handle_shutdown_start(m);
    return;

  case MSG_PING:
    handle_ping((MPing*)m);
    return;

  default:
    assert(0);
  }

}



void MDS::ms_handle_failure(Message *m, const entity_inst_t& inst) 
{
  mds_lock.Lock();
  dout(10) << "handle_ms_failure to " << inst << " on " << *m << endl;
  
  if (m->get_type() == MSG_CLIENT_RECONNECT) 
    server->client_reconnect_failure(m->get_dest().num());

  delete m;
  mds_lock.Unlock();
}





void MDS::handle_ping(MPing *m)
{
  dout(10) << " received ping from " << m->get_source() << " with seq " << m->seq << endl;

  messenger->send_message(new MPingAck(m),
                          m->get_source_inst());
  
  delete m;
}




class C_LogClientmap : public Context {
  ClientMap *clientmap;
  version_t cmapv;
public:
  C_LogClientmap(ClientMap *cm, version_t v) : 
    clientmap(cm), cmapv(v) {}
  void finish(int r) {
    clientmap->set_committed(cmapv);
    list<Context*> ls;
    clientmap->take_commit_waiters(cmapv, ls);
    finish_contexts(ls);
  }
};

void MDS::log_clientmap(Context *c)
{
  dout(10) << "log_clientmap " << clientmap.get_version() << endl;

  bufferlist bl;
  clientmap.encode(bl);

  clientmap.set_committing(clientmap.get_version());
  clientmap.add_commit_waiter(c);

  mdlog->submit_entry(new EClientMap(bl, clientmap.get_version()),
		      new C_LogClientmap(&clientmap, clientmap.get_version()));
}
