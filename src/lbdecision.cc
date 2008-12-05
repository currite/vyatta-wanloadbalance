/*
 * Module: lbdecision.cc
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <iostream>
#include "lbdata.hh"
#include "lbdecision.hh"

using namespace std;

/*
iptables -t mangle -N ISP1
iptables -t mangle -A ISP1 -j CONNMARK --set-mark 1
iptables -t mangle -A ISP1 -j MARK --set-mark 1
iptables -t mangle -A ISP1 -j ACCEPT

iptables -t mangle -N ISP2
iptables -t mangle -A ISP2 -j CONNMARK --set-mark 2
iptables -t mangle -A ISP2 -j MARK --set-mark 2
iptables -t mangle -A ISP2 -j ACCEPT


#THIS APPEARS TO ROUGHLY WORK BELOW, AND CAN BE SET UP WITH SPECIFIC FILTERS.
iptables -t mangle -A PREROUTING -i eth0 -m statistic --mode nth --every 2 --packet 0 -j ISP1
iptables -t mangle -A PREROUTING -i eth0 -j ISP2

#iptables -t mangle -A PREROUTING -i eth0 -m state --state NEW -m statistic --mode random --probability .01 -j MARK --set-mark 1
#iptables -t mangle -A PREROUTING -i eth0 -j MARK --set-mark 2

iptables -t raw -N NAT_CONNTRACK
iptables -t raw -A NAT_CONNTRACK -j ACCEPT
iptables -t raw -I PREROUTING 1 -j NAT_CONNTRACK
iptables -t raw -I OUTPUT 1 -j NAT_CONNTRACK
ip ro add table 10 default via 192.168.1.2  dev eth1
ip ru add fwmark 1 table 10
ip ro fl ca
ip ro add table 20 default via 192.168.2.2 dev eth2
ip ru add fwmark 2 table 20
ip ro fl ca 

*/


/**
 *
 *
 **/
LBDecision::LBDecision(bool debug) : 
  _debug(debug)
{

}

/**
 *
 *
 **/
LBDecision::~LBDecision()
{
}

/**
 *
 *
 **/
void
LBDecision::init(LBData &lbdata)
{
  //here is where we set up iptables and policy routing for the interfaces
  /*
    iptables -t mangle -N ISP1
    iptables -t mangle -A ISP1 -j CONNMARK --set-mark 1
    iptables -t mangle -A ISP1 -j MARK --set-mark 1
    iptables -t mangle -A ISP1 -j ACCEPT
   */

  char buf[20];

  /*
    do we need: 
iptables -t raw -N NAT_CONNTRACK
iptables -t raw -A NAT_CONNTRACK -j ACCEPT
iptables -t raw -I PREROUTING 1 -j NAT_CONNTRACK
iptables -t raw -I OUTPUT 1 -j NAT_CONNTRACK

if so then this stuff goes here!
   */


  //note: doesn't appear to clean up rule table, may need to individually erase each rule
  //  execute(string("ip rule flush"));

  string stdout;
  //set up special nat rules
  if (lbdata._disable_source_nat == false) {
    execute(string("iptables -t nat -N WANLOADBALANCE"), stdout);
    execute(string("iptables -t nat -F WANLOADBALANCE"), stdout);
    execute(string("iptables -t nat -D VYATTA_PRE_SNAT_HOOK -j WANLOADBALANCE"), stdout);
    execute(string("iptables -t nat -I VYATTA_PRE_SNAT_HOOK 1 -j WANLOADBALANCE"), stdout);
  }
  //set up the conntrack table
  execute(string("iptables -t raw -N NAT_CONNTRACK"), stdout);
  execute(string("iptables -t raw -F NAT_CONNTRACK"), stdout);
  execute(string("iptables -t raw -A NAT_CONNTRACK -j ACCEPT"), stdout);
  execute(string("iptables -t raw -D PREROUTING 1"), stdout);
  execute(string("iptables -t raw -I PREROUTING 1 -j NAT_CONNTRACK"), stdout);


  LBData::InterfaceHealthIter iter = lbdata._iface_health_coll.begin();
  while (iter != lbdata._iface_health_coll.end()) {
    string iface = iter->first;
    
    int ct = iter->second._interface_index;

    sprintf(buf,"%d",ct);

    execute(string("iptables -t mangle -N ISP_") + buf, stdout);
    execute(string("iptables -t mangle -F ISP_") + buf, stdout);
    execute(string("iptables -t mangle -A ISP_") + buf + " -j CONNMARK --set-mark " + buf, stdout);
    execute(string("iptables -t mangle -A ISP_") + buf + " -j MARK --set-mark " + buf, stdout);

    //NOTE, WILL NEED A WAY TO CLEAN UP THIS RULE ON RESTART...
    execute(string("iptables -t mangle -A ISP_") + buf + " -j ACCEPT", stdout);
    
    //    insert_default(string("ip route replace table ") + buf + " default dev " + iface + " via " + iter->second._nexthop, ct);
    //need to force the entry on restart as the configuration may have changed.
    if (iter->second._nexthop == "dhcp") {
      string nexthop = fetch_iface_nexthop(iface);
      execute(string("ip route replace table ") + buf + " default dev " + iface + " via " + nexthop, stdout);
    }
    else {
      execute(string("ip route replace table ") + buf + " default dev " + iface + " via " + iter->second._nexthop, stdout);
    }

    execute(string("ip rule delete table ") + buf, stdout);

    char hex_buf[40];
    sprintf(hex_buf,"%X",ct);
    execute(string("ip rule add fwmark ") + hex_buf + " table " + buf, stdout);

    if (lbdata._disable_source_nat == false) {
      iter->second._address = fetch_iface_addr(iface);
      execute(string("iptables -t nat -A WANLOADBALANCE -m connmark --mark ") + buf + " -j SNAT --to-source " + iter->second._address, stdout);
    }
    ++iter;
  }
  execute("ip route flush cache", stdout);
}

/**
 * Need to do two things here, check the interfaces for new nexthops and update the routing tables,
 * and get a new address to update the source nat with.
 *
 **/
void
LBDecision::update_paths(LBData &lbdata)
{
  string stdout;
  if (lbdata._disable_source_nat == false) {
    //first let's remove the entry
    LBData::InterfaceHealthIter iter = lbdata._iface_health_coll.begin();
    while (iter != lbdata._iface_health_coll.end()) {
      if (iter->second._nexthop == "dhcp") {
	string iface = iter->first;
	string new_addr = fetch_iface_addr(iface);
	if (new_addr != iter->second._address) {
	  char buf[20];
	  sprintf(buf,"%d",iter->second._interface_index);
	  execute(string("iptables -t nat -D WANLOADBALANCE -m connmark --mark ") + buf + " -j SNAT --to-source " + iter->second._address, stdout);
	  execute(string("iptables -t nat -A WANLOADBALANCE -m connmark --mark ") + buf + " -j SNAT --to-source " + new_addr, stdout);
	  iter->second._address = new_addr;

	  //now let's update the nexthop here in the route table
	  string nexthop = fetch_iface_nexthop(iface);
	  execute(string("ip route replace table ") + buf + " default dev " + iface + " via " + nexthop, stdout);
	}
      }
      ++iter;
    }
  }
}

/**
 * only responsible for 

iptables -t mangle -A PREROUTING -i eth0 -m state --state NEW -m statistic --mode random --probability .01 -j MARK --set-mark 1
iptables -t mangle -A PREROUTING -i eth0 -j MARK --set-mark 2

 *
 *
 *
 **/
void
LBDecision::run(LBData &lb_data)
{
  if (_debug) {
    cout << "LBDecision::run(), starting decision" << endl;
  }

  string stdout;

  if (_debug) {
    cout << "LBDecision::run(), state changed, applying new rule set" << endl;
  }

  //now reapply the routing tables.
  LBData::InterfaceHealthIter h_iter = lb_data._iface_health_coll.begin();
  while (h_iter != lb_data._iface_health_coll.end()) {
    if (h_iter->second._is_active == true) {
      char buf[40];
      sprintf(buf,"%d",h_iter->second._interface_index);
      insert_default(string("ip route replace table ") + buf + " default dev " + h_iter->first + " via " + h_iter->second._nexthop, h_iter->second._interface_index);
    }
    else {
      //right now replace route, but don't delete until race condition is resolved
      //	execute(string("ip route delete ") + route_str);
    }
    ++h_iter;
  }

  //first determine if we need to alter the rule set
  if (!lb_data.state_changed()) {
    return;
  }

  //then if we do, flush all
  execute("iptables -t mangle -F PREROUTING", stdout);

  //and compute the new set and apply
  LBData::LBRuleIter iter = lb_data._lb_rule_coll.begin();
  while (iter != lb_data._lb_rule_coll.end()) {
    //NEED TO HANDLE APPLICATION SPECIFIC DETAILS
    string app_cmd = get_application_cmd(iter->second);

    if (iter->second._exclude == true) {
      execute(string("iptables -t mangle -A PREROUTING ") + app_cmd + " -j ACCEPT", stdout);
    }
    else {
      map<int,float> weights = get_new_weights(lb_data,iter->second);
      
      if (weights.empty()) {
	//no rules here!
      }
      else {
	char fbuf[20],dbuf[20];
	map<int,float>::iterator w_iter = weights.begin();
	for (w_iter = weights.begin(); w_iter != (--weights.end()); w_iter++) {
	  sprintf(fbuf,"%f",w_iter->second);
	  sprintf(dbuf,"%d",w_iter->first);
	  execute(string("iptables -t mangle -A PREROUTING ") + app_cmd + " -m state --state NEW -m statistic --mode random --probability " + fbuf + " -j ISP_" + dbuf, stdout);
	}
	sprintf(dbuf,"%d",(--weights.end())->first);
	execute(string("iptables -t mangle -A PREROUTING ") + app_cmd + " -m state --state NEW -j ISP_" + dbuf, stdout);
	execute(string("iptables -t mangle -A PREROUTING ") + app_cmd + " -j CONNMARK --restore-mark", stdout);
      }
    }
    ++iter;
    continue;
  }
}

/**
 *
 *
 **/
void
LBDecision::shutdown(LBData &data)
{
  string stdout;

  //then if we do, flush all
  execute("iptables -t mangle -F PREROUTING", stdout);

  //clear out nat as well
  execute("iptables -t nat -F WANLOADBALANCE", stdout);
  execute("iptables -t nat -D VYATTA_PRE_SNAT_HOOK -j WANLOADBALANCE", stdout);


  //remove the policy entries
  LBData::InterfaceHealthIter h_iter = data._iface_health_coll.begin();
  while (h_iter != data._iface_health_coll.end()) {
    char buf[40];
    sprintf(buf,"%d",h_iter->second._interface_index);
    
    execute(string("ip rule del table ") + buf, stdout);

    //need to delete ip rule here as well!

    ++h_iter;
  }
}

/**
 *
 **/
int
LBDecision::execute(std::string cmd, std::string &stdout, bool read)
{
  int err = 0;

  if (_debug) {
    cout << "LBDecision::execute(): applying command to system: " << cmd << endl;
    syslog(LOG_DEBUG, "LBDecision::execute(): applying command to system: %s",cmd.c_str());
  }
 
  string dir = "w";
  if (read == true) {
    dir = "r";
  }
  FILE *f = popen(cmd.c_str(), dir.c_str());
  if (f) {
    if (read == true) {
      fflush(f);
      char *buf = NULL;
      size_t len = 0;
      size_t read_len = 0;
      while ((read_len = getline(&buf, &len, f)) != (size_t)-1) {
	stdout += string(buf) + " ";
      }

      if (buf) {
	free(buf);
      }
    }
    err = pclose(f);
  }
  return err;
}

/**
 *
 **/
map<int,float> 
LBDecision::get_new_weights(LBData &data, LBRule &rule)
{
  map<int,float> weights;
  int group = 0;
  LBRule::InterfaceDistIter iter = rule._iface_dist_coll.begin();
  while (iter != rule._iface_dist_coll.end()) {
    if (_debug) {
      cout << "LBDecision::get_new_weights(): " << iter->first << " is active: " << (data.is_active(iter->first) ? "true" : "false") << endl;
    }

    int ct = 0;
    LBData::InterfaceHealthIter h_iter = data._iface_health_coll.find(iter->first);
    if (h_iter != data._iface_health_coll.end()) {
      ct = h_iter->second._interface_index;
    }

    if (rule._failover == true) { //add single entry if active
      if (data.is_active(iter->first)) {
	//select the active interface that has the highest weight
	if (iter->second > group) {
	  map<int,float>::iterator w_iter = weights.begin();
	  while (w_iter != weights.end()) {
	    w_iter->second = 0.; //zero out previous weight
	    ++w_iter;
	  }
	}
	weights.insert(pair<int,float>(ct,iter->second));
	group = iter->second;
      }
      else {
	weights.insert(pair<int,float>(ct,0.));	
      }
    }
    else {
      if (data.is_active(iter->first)) {
	weights.insert(pair<int,float>(ct,iter->second));
	group += iter->second;
      }
      else {
	weights.insert(pair<int,float>(ct,0.));
      }
    }
    ++iter;
  }

  if (group == 0) {
    weights.erase(weights.begin(),weights.end());
  }
  else {
    //now weight the overall distribution
    map<int,float>::iterator w_iter = weights.begin();
    while (w_iter != weights.end()) {
      float w = 0.;
      if (w_iter->second > 0.) { //can only be an integer value here
	w = float(w_iter->second) / float(group);
      }
      group -= (int)w_iter->second;   //I THINK THIS NEEDS TO BE ADJUSTED TO THE OVERALL REMAINING VALUES. which is this...
      if (w < .01) {
	weights.erase(w_iter++);
	continue;
      }
      w_iter->second = w;
      ++w_iter;
    }
  }
  return weights;
}

/**
 *
 *
 **/
string
LBDecision::get_application_cmd(LBRule &rule)
{
  string filter;

  if (rule._in_iface.empty() == false) {
    filter += "-i " + rule._in_iface + " ";
  }

  if (rule._proto.empty() == false) {
    filter += "--proto " + rule._proto + " ";
  }
  
  if (rule._proto == "icmp") {
    filter += "--icmp-type any ";
  }

  if (rule._s_addr.empty() == false) {
    bool negate_flag = false;
    string tmp(rule._s_addr);
    if (tmp.find("!") != string::npos) {
      negate_flag = true;
      tmp = tmp.substr(1,tmp.length()-1);
    }

    if (tmp.find("-") != string::npos) {
      if (negate_flag) {
	filter += "-m iprange ! --src-range " + tmp + " ";
      }
      else {
	filter += "-m iprange --src-range " + tmp + " ";
      }
    }
    else {
      if (negate_flag) {
	filter += "--source ! " + tmp + " ";
      }
      else {
	filter += "--source " + tmp + " ";
      }
    }
  }
  
  if (rule._d_addr.empty() == false) {
    bool negate_flag = false;
    string tmp(rule._d_addr);
    if (tmp.find("!") != string::npos) {
      negate_flag = true;
      tmp = tmp.substr(1,tmp.length()-1);
    }

    if (tmp.find("-") != string::npos) {
      if (negate_flag) {
	filter += "-m iprange ! --dst-range " + tmp + " ";
      }
      else {
	filter += "-m iprange --dst-range " + tmp + " ";
      }
    }
    else {
      if (negate_flag) {
	filter += "--destination ! " + tmp + " ";
      }
      else {
	filter += "--destination " + tmp + " ";
      }
    }
  }

  if (rule._proto == "udp" || rule._proto == "tcp") {
    if (rule._s_port.empty() == false && rule._s_port_ipt.empty() == true) {
      filter += "-m multiport --source-port " + rule._s_port + " ";
    }
    else if (rule._s_port_ipt.empty() == false) {
      filter += rule._s_port_ipt + " ";
    }

    if (rule._d_port.empty() == false && rule._d_port_ipt.empty() == true) {
      filter += "-m multiport --destination-port " + rule._d_port + " ";
    }
    else if (rule._d_port_ipt.empty() == false) {
      filter += rule._d_port_ipt + " ";
    }
  }

  return filter;
}


/**
 * Check for the presence of a route entry in the policy table. Note this 
 * should be replaced by netlink in the next release.
 **/
void
LBDecision::insert_default(string cmd, int table)
{
  string stdout;
  char buf[40];
  string showcmd("ip route show table ");
  sprintf(buf,"%d",table);
  showcmd += string(buf);
  execute(showcmd,stdout,true);

  //  cout << "LBDecision::insert_default(stdout): '" << stdout << "'" << endl;

  if (stdout.empty() == true) {
    //    cout << "LBDecision::insert_default(cmd): " << cmd << endl;
    execute(cmd,stdout);
  }
}

/**
 * currently only reads the nexthop as maintained by the dhcp client
 **/
string
LBDecision::fetch_iface_nexthop(const string &iface)
{
  string file("/var/run/vyatta/dhclient/dhclient-script-router-"+iface);
  FILE *fp = fopen(file.c_str(),"r");
  if (fp) {
    char str[1025];
    int ct = 0;
    if ((ct = fread(str, 1, 1024, fp)) > 0) {
      return string(str);
    }
    fclose(fp);
  }
  return string("");
}

/**
 * Fetch interface configuration 
 **/
string
LBDecision::fetch_iface_addr(const string &iface)
{
  struct ifreq ifr;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    syslog(LOG_ERR, "Error obtaining socket");
    return string("");
  }
  
  int ct = 2;
  //try twice to retrieve this before failing
  while (ct > 0) {
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
      struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
      struct in_addr in;
      in.s_addr = sin->sin_addr.s_addr;
      char *tmp_buf = inet_ntoa(in);
      close(fd);
      return string(tmp_buf);
    }
    usleep(500 * 1000); //.5 second sleep
    --ct;
  }
  close(fd);
  return string("");
}


