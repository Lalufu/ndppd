// ndppd - NDP Proxy Daemon
// Copyright (C) 2011  Daniel Adolfsson <daniel@priv.nu>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include <cstdlib>

#include <iostream>
#include <fstream>
#include <string>
#include <memory>

#include <getopt.h>
#include <sys/time.h>

#include <sys/types.h>
#include <unistd.h>

#include "ndppd.h"

using namespace ndppd;

int daemonize()
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid > 0)
        exit(0);

    pid_t sid = setsid();

    if (sid < 0)
        return -1;

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return 0;
}

bool configure(const std::string &path)
{
    ptr<conf> cf;

    if (!(cf = conf::load(path)))
        return false;

    std::vector<ptr<conf> >::const_iterator p_it;

    std::vector<ptr<conf> > proxies(cf->find("proxy"));

    for (p_it = proxies.begin(); p_it != proxies.end(); p_it++) {
        ptr<conf> pr_cf = *p_it, x_cf;

        if (pr_cf->value() == "") {
            logger::error() << "'proxy' section is missing interface name";
            return false;
        }

        ptr<proxy> pr = proxy::open(pr_cf->value());

        if (!pr) {
            logger::error() << "Configuration failed for proxy '" << pr_cf->value() << "'";
            return false;
        }

        if (!(x_cf = (*pr_cf)["router"]))
            pr->router(true);
        else
            pr->router(x_cf->bool_value());

        if (!(x_cf = (*pr_cf)["ttl"]))
            pr->ttl(30000);
        else
            pr->ttl(x_cf->int_value());

        if (!(x_cf = (*pr_cf)["timeout"]))
            pr->timeout(500);
        else
            pr->timeout(x_cf->int_value());

        std::vector<ptr<conf> >::const_iterator r_it;

        std::vector<ptr<conf> > rules(pr_cf->find("rule"));

        for (r_it = rules.begin(); r_it != rules.end(); r_it++) {
            ptr<conf> ru_cf = *r_it;

            if (ru_cf->value() == "") {
                logger::error() << "'rule' is missing an IPv6 address/net";
                return false;
            }

            address addr(ru_cf->value());

            if (!(x_cf = (*ru_cf)["iface"])) {
                if (addr.prefix() <= 120) {
                    logger::warning() << "Static rule prefix /" << addr.prefix() << " <= 120 - is this what you want?";
                    pr->add_rule(addr);
                }
            } else if (x_cf->value() == "") {
                logger::error() << "'iface' expected an interface name or 'auto' as argument";
            } else {
                pr->add_rule(addr, iface::open_ifd(x_cf->value()));
            }
        }
    }

    return true;
}

int main(int argc, char *argv[], char *env[])
{
    std::string config_path("/etc/ndppd.conf");
    std::string pidfile;
    std::string verbosity;
    bool daemon = false;

    while (1) {
        int c, opt;

        static struct option long_options[] =
        {
            { "config",     1, 0, 'c' },
            { "daemon",     0, 0, 'd' },
            { "verbose",    1, 0, 'v' },
            { 0, 0, 0, 0}
        };

        c = getopt_long(argc, argv, "c:dp:v::", long_options, &opt);

        if (c == -1)
            break;

        switch (c) {
        case 'c':
            config_path = optarg;
            break;

        case 'd':
            daemon = true;
            break;

        case 'p':
            pidfile = optarg;
            break;

        case 'v':
            if (optarg) {
                if (!logger::verbosity(optarg))
                    logger::error() << "Unknown verbosity level '" << optarg << "'";
            } else {
                logger::max_pri(LOG_INFO);
            }
            break;
        }
    }

    if (daemon) {
        logger::syslog(true);

        if (daemonize() < 0) {
            logger::error() << "Failed to daemonize process";
            return 1;
        }
    }

    if (!pidfile.empty()) {
        std::ofstream pf;
        pf.open(pidfile.c_str(), std::ios::out | std::ios::trunc);
        pf << getpid() << std::endl;
        pf.close();
    }

    logger::info().force_log()
        << "ndppd (NDP Proxy Daemon) version " NDPPD_VERSION << logger::endl
        << "Using configuration file '" << config_path << "'";

    // Load configuration.

    if (!configure(config_path))
        return -1;

    // Time stuff.

    struct timeval t1, t2;

    gettimeofday(&t1, 0);

    while (iface::poll_all() >= 0) {
        int elapsed_time;
        gettimeofday(&t2, 0);

        elapsed_time =
            ((t2.tv_sec  - t1.tv_sec)  * 1000) +
            ((t2.tv_usec - t1.tv_usec) / 1000);

        t1.tv_sec  = t2.tv_sec;
        t1.tv_usec = t2.tv_usec;

        session::update_all(elapsed_time);
    }

    logger::error() << "iface::poll_all() failed";

    return 0;
}

