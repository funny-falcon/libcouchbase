/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "config.h"
#include <sys/types.h>
#include <libcouchbase/couchbase.h>

#include <iostream>
#include <map>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <getopt.h>

#include "tools/commandlineparser.h"


using namespace std;

class Configuration
{
public:
    Configuration() : host(),
        maxKey(1000),
        iterations(1000),
        fixedSize(true),
        setprc(33),
        prefix(""),
        maxSize(1024),
        numThreads(1) {
        // @todo initialize the random sequence in seqno
        data = static_cast<void *>(new char[maxSize]);
    }

    ~Configuration() {
        delete []static_cast<char *>(data);
    }

    const char *getHost() const {
        if (host.length() > 0) {
            return host.c_str();
        }
        return NULL;
    }

    void setHost(const char *val) {
        host.assign(val);
    }

    const char *getUser() {
        if (user.length() > 0) {
            return user.c_str();
        }
        return NULL;
    }

    const char *getPasswd() {
        if (passwd.length() > 0) {
            return passwd.c_str();
        }
        return NULL;
    }

    void setPassword(const char *p) {
        passwd.assign(p);
    }

    void setUser(const char *u) {
        user.assign(u);
    }

    const char *getBucket() {
        if (bucket.length() > 0) {
            return bucket.c_str();
        }
        return NULL;
    }

    void setBucket(const char *b) {
        bucket.assign(b);
    }

    void setIterations(uint32_t val) {
        iterations = val;
    }

    void setMaxKeys(uint32_t val) {
        maxKey = val;
    }

    void setKeyPrefix(const char *val) {
        prefix.assign(val);
    }

    void setNumThreads(uint32_t val) {
        numThreads = val;
    }

    void *data;

    std::string host;
    std::string user;
    std::string passwd;
    std::string bucket;

    uint32_t maxKey;
    uint32_t iterations;
    bool fixedSize;
    uint32_t setprc;
    std::string prefix;
    uint32_t maxSize;
    uint32_t numThreads;

} config;

extern "C" {
    static void storageCallback(lcb_t, const void *,
                                lcb_storage_t, lcb_error_t,
                                const lcb_store_resp_t *);

    static void getCallback(lcb_t, const void *, lcb_error_t,
                            const lcb_get_resp_t *);

    static void timingsCallback(lcb_t, const void *,
                                lcb_timeunit_t, lcb_uint32_t,
                                lcb_uint32_t, lcb_uint32_t,
                                lcb_uint32_t);
}

class ThreadContext
{
public:
    ThreadContext() :
        currSeqno(0), instance(NULL) {
        // @todo fill the random seqnos

    }
    ~ThreadContext() {
        if (instance != NULL) {
            lcb_destroy(instance);
        }
    }
    bool create(void) {
        lcb_io_opt_t io;
        if (lcb_create_io_ops(&io, NULL) != LCB_SUCCESS) {
            std::cerr << "Failed to create an IO instance" << std::endl;
            return false;
        }

        struct lcb_create_st options(config.getHost(), config.getUser(),
                                     config.getPasswd(), config.getBucket(),
                                     io);

        if (lcb_create(&instance, &options) == LCB_SUCCESS) {
            (void)lcb_set_store_callback(instance, storageCallback);
            (void)lcb_set_get_callback(instance, getCallback);
            return true;
        } else {
            return false;
        }
    }

    bool connect(void) {
        if ((error = lcb_connect(instance)) != LCB_SUCCESS) {
            std::cerr << "Failed to connect: "
                      << lcb_strerror(instance, error) << std::endl;
            return false;
        }

        lcb_wait(instance);
        error = lcb_get_last_error(instance);
        if (error != LCB_SUCCESS) {
            std::cerr << "Failed to connect: "
                      << lcb_strerror(instance, error) << std::endl;
            return false;
        }

        return true;
    }

    bool run(bool loop) {
        do {
            bool timings = true;
            if ((error = lcb_enable_timings(instance)) != LCB_SUCCESS) {
                std::cerr << "Failed to enable timings!: "
                          << lcb_strerror(instance, error) << std::endl;
                timings = false;
            }

            bool pending = false;
            for (uint32_t ii = 0; ii < config.iterations; ++ii) {
                std::string key;
                generateKey(key);

                lcb_uint32_t flags = 0;
                lcb_uint32_t exp = 0;

                if (config.setprc > 0 && (nextSeqno() % 100) < config.setprc) {
                    lcb_store_cmd_t item(LCB_SET, key.c_str(), key.length(),
                                         config.data, config.maxSize,
                                         flags, exp);
                    lcb_store_cmd_t *items[1] = { &item };
                    lcb_store(instance, this, 1, items);
                } else {
                    lcb_get_cmd_t item;
                    memset(&item, 0, sizeof(item));
                    item.v.v0.key = key.c_str();
                    item.v.v0.nkey = key.length();
                    lcb_get_cmd_t *items[1] = { &item };
                    error = lcb_get(instance, this, 1, items);
                    if (error != LCB_SUCCESS) {
                        std::cerr << "Failed to get item: "
                                  << lcb_strerror(instance, error) << std::endl;
                    }
                }

                if (ii % 10 == 0) {
                    lcb_wait(instance);
                } else {
                    lcb_wait(instance);
                    //pending = true;
                }
            }

            if (pending) {
                lcb_wait(instance);
            }

            if (timings) {
                dumpTimings("Run");
                lcb_disable_timings(instance);
            }
        } while (loop);

        return true;
    }

    bool populate(uint32_t start, uint32_t stop) {
        bool timings = true;
        if ((error = lcb_enable_timings(instance)) != LCB_SUCCESS) {
            std::cerr << "Failed to enable timings!: "
                      << lcb_strerror(instance, error) << std::endl;
            timings = false;
        }

        for (uint32_t ii = start; ii < stop; ++ii) {
            std::string key;
            generateKey(key, ii);

            lcb_store_cmd_t item(LCB_SET, key.c_str(), key.length(),
                                 config.data, config.maxSize);
            lcb_store_cmd_t *items[1] = { &item };
            error = lcb_store(instance, this, 1, items);
            if (error != LCB_SUCCESS) {
                std::cerr << "Failed to store item: "
                          << lcb_strerror(instance, error) << std::endl;
            }
            lcb_wait(instance);
            if (error != LCB_SUCCESS) {
                std::cerr << "Failed to store item: "
                          << lcb_strerror(instance, error) << std::endl;
            }
        }

        if (timings) {
            dumpTimings("Populate");
            lcb_disable_timings(instance);
        }

        return true;
    }

protected:
    // the callback methods needs to be able to set the error handler..
    friend void storageCallback(lcb_t, const void *,
                                lcb_storage_t, lcb_error_t,
                                const lcb_store_resp_t *);
    friend void getCallback(lcb_t, const void *, lcb_error_t,
                            const lcb_get_resp_t *);

    void setError(lcb_error_t e) {
        error = e;
    }

    void dumpTimings(std::string header) {
        std::stringstream ss;
        ss << header << std::endl;
        ss << "              +---------+---------+---------+---------+" << std::endl;
        lcb_get_timings(instance, reinterpret_cast<void *>(&ss),
                        timingsCallback);
        ss << "              +----------------------------------------" << endl;
        std::cout << ss.str();
    }

private:
    uint32_t nextSeqno() {
        uint32_t ret = seqno[currSeqno];
        currSeqno += ret;
        if (currSeqno > 8191) {
            currSeqno &= 0xff;
        }
        return ret;
    }

    void generateKey(std::string &key,
                     uint32_t ii = static_cast<uint32_t>(-1)) {
        if (ii == static_cast<uint32_t>(-1)) {
            // get random key
            ii = nextSeqno() % config.maxKey;
        }

        std::stringstream ss;
        ss << config.prefix << ":" << ii;
        key.assign(ss.str());
    }

    uint32_t seqno[8192];
    uint32_t currSeqno;

    lcb_t instance;
    lcb_error_t error;
};

static void storageCallback(lcb_t, const void *cookie,
                            lcb_storage_t, lcb_error_t error,
                            const lcb_store_resp_t *)
{
    ThreadContext *tc;
    tc = const_cast<ThreadContext *>(reinterpret_cast<const ThreadContext *>(cookie));
    tc->setError(error);
}

static void getCallback(lcb_t, const void *cookie,
                        lcb_error_t error,
                        const lcb_get_resp_t *)
{
    ThreadContext *tc;
    tc = const_cast<ThreadContext *>(reinterpret_cast<const ThreadContext *>(cookie));
    tc->setError(error);

}

static void timingsCallback(lcb_t instance, const void *cookie,
                            lcb_timeunit_t timeunit,
                            lcb_uint32_t min,
                            lcb_uint32_t max,
                            lcb_uint32_t total,
                            lcb_uint32_t maxtotal)
{
    std::stringstream *ss =
        const_cast<std::stringstream *>(reinterpret_cast<const std::stringstream *>(cookie));
    char buffer[1024];
    int offset = sprintf(buffer, "[%3u - %3u]", min, max);

    switch (timeunit) {
    case LCB_TIMEUNIT_NSEC:
        offset += sprintf(buffer + offset, "ns");
        break;
    case LCB_TIMEUNIT_USEC:
        offset += sprintf(buffer + offset, "us");
        break;
    case LCB_TIMEUNIT_MSEC:
        offset += sprintf(buffer + offset, "ms");
        break;
    case LCB_TIMEUNIT_SEC:
        offset += sprintf(buffer + offset, "s");
        break;
    default:
        ;
    }

    int num = static_cast<int>(static_cast<float>(40.0) * static_cast<float>(total) / static_cast<float>(maxtotal));

    offset += sprintf(buffer + offset, " |");
    for (int ii = 0; ii < num; ++ii) {
        offset += sprintf(buffer + offset, "#");
    }

    offset += sprintf(buffer + offset, " - %u\n", total);
    *ss << buffer;
    (void)cookie;
    (void)maxtotal;
    (void)instance;
}

static void handle_options(int argc, char **argv)
{
    Getopt getopt;
    getopt.addOption(new CommandLineOption('?', "help", false,
                                           "Print this help text"));
    getopt.addOption(new CommandLineOption('h', "host", true,
                                           "Hostname to connect to"));
    getopt.addOption(new CommandLineOption('b', "bucket", true,
                                           "Bucket to use"));
    getopt.addOption(new CommandLineOption('u', "user", true,
                                           "Username for the rest port"));
    getopt.addOption(new CommandLineOption('P', "password", true,
                                           "password for the rest port"));
    getopt.addOption(new CommandLineOption('i', "iterations", true, "Number of iterations to run"));
    getopt.addOption(new CommandLineOption('I', "num-items", true, "Number of items to operate on"));
    getopt.addOption(new CommandLineOption('p', "key-prefix", true, "Use the following prefix for keys"));
    getopt.addOption(new CommandLineOption('t', "num-threads", true, "The number of threads to use"));
    /* getopt.addOption(new CommandLineOption()); */
    /* getopt.addOption(new CommandLineOption()); */

    if (!getopt.parse(argc, argv)) {
        getopt.usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    std::vector<CommandLineOption *>::iterator iter;
    for (iter = getopt.options.begin(); iter != getopt.options.end(); ++iter) {
        if ((*iter)->found) {
            switch ((*iter)->shortopt) {
            case 'h' :
                config.setHost((*iter)->argument);
                break;

            case 'b' :
                config.setBucket((*iter)->argument);
                break;

            case 'u' :
                config.setUser((*iter)->argument);
                break;

            case 'P' :
                config.setPassword((*iter)->argument);
                break;

            case 'i' :
                config.setIterations(atoi((*iter)->argument));
                break;

            case 'I':
                config.setMaxKeys(atoi((*iter)->argument));
                break;

            case 'p' :
                config.setKeyPrefix((*iter)->argument);
                break;

            case 't':
                config.setNumThreads(atoi((*iter)->argument));
                break;

            case '?':
                getopt.usage(argv[0]);
                exit(EXIT_FAILURE);
            default:
                abort();
            }

        }
    }
}

/**
 * Program entry point
 * @param argc argument count
 * @param argv argument vector
 * @return 0 success, 1 failure
 */
int main(int argc, char **argv)
{
    handle_options(argc, argv);

    ThreadContext ctx;
    if (!ctx.create() || !ctx.connect()) {
        return 1;
    }

    ctx.populate(0, config.maxKey);
    ctx.run(true);

    return 0;
}
