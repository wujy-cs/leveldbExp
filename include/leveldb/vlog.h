//
// Created by wujy on 8/21/18.
//

#ifndef MULTI_BF_LSM_VLOG_H
#define MULTI_BF_LSM_VLOG_H

#include "db.h"
#include <string>
#include <iostream>
#include "unistd.h"
#include "statistics.h"
#include "env.h"
#include <vector>
#include "threadpool.h"
#include <condition_variable>
#include <mutex>

using std::string;

namespace leveldb {

    struct LEVELDB_EXPORT VlogOptions : Options {
        int numThreads;
    };

    class SepDB {
    public:
        SepDB(VlogOptions &options, const std::string &dbname, const std::string &vlogname, Status &s) : rawOptions(
                options) {
            threadPool = new ThreadPool(rawOptions.numThreads);
            s = DB::Open(options, dbname, &indexDB);
            if (!s.ok()) {
                std::cout << s.ToString() << std::endl;
                return;
            }
            vlog = fopen(vlogname.c_str(), "a+");
        }

        ~SepDB() {
            fclose(vlog);
            delete indexDB;
        }

        static Status Open(VlogOptions options, const std::string &dbname, const std::string &vlogname, SepDB **db) {
            Status s;
            *db = new SepDB(options, dbname, vlogname, s);
            return s;
        }

        /*
         * indexDB: <key,offset+value size>
         * vlog: <key size, value size, key, value>
         * use '$' to seperate offset and value size, key size and value size, value size and key
        */
        Status Put(const WriteOptions writeOptions, const string &key, const string &val) {
            uint64_t startMicro = NowMiros();
            long keySize = key.size();
            long valueSize = val.size();
            string keySizeStr = std::to_string(keySize);
            string valueSizeStr = std::to_string(valueSize);

            string vlogStr =
                    keySizeStr + "$" + valueSizeStr + "$" + key + val; // | key size | value size | key | value |
            fwrite(vlogStr.c_str(), vlogStr.size(), 1, vlog);

            long vlogOffset = ftell(vlog) - val.size();
            string vlogOffsetStr = std::to_string(vlogOffset);
            string indexStr = vlogOffsetStr + "$" + valueSizeStr;
            Status s = indexDB->Put(writeOptions, key, indexStr);
            STATS::timeAndCount(STATS::getInstance()->writeVlogStat, startMicro, NowMiros());
            return s;
        }

        /*
         * Get value offset and value size from indexDB
         * Get value from vlog
        */
        Status Get(const ReadOptions readOptions, const string &key, string *val) {
            uint64_t startMicro = NowMiros();
            string valueInfo;
            Status s = indexDB->Get(readOptions, key, &valueInfo);
            if (!s.ok()) return s;
            s = readValue(valueInfo, val);
            STATS::timeAndCount(STATS::getInstance()->readVlogStat, startMicro, NowMiros());
            return s;
        }

        Status Delete(const WriteOptions writeOptions, const string &key) {
            uint64_t startMicro = NowMiros();
            Status s = indexDB->Delete(writeOptions, key);
            STATS::timeAndCount(STATS::getInstance()->writeVlogStat, startMicro, NowMiros());
            return s;
        }

        // multi-threading range query
        size_t Scan(const ReadOptions readOptions, const string &start, size_t num, std::vector<string> &keys,
                    std::vector<string> &values) {
            uint64_t startMicro = NowMiros();
            size_t i;
            if (keys.size() < num)
                keys.resize(num);
            if (values.size() < num)
                values.resize(num);
            std::vector<string> valueInfos(num);
//            std::atomic<int> keysInProgress;
//            keysInProgress = 0;

            // use index tree iter to get value info
            auto iter = indexDB->NewIterator(readOptions);
            iter->Seek(start);

            //for main thread waiting
            std::future<Status> s;
            for (i = 0; i < num && iter->Valid(); i++) {
                keys[i] = iter->key().ToString();
                valueInfos[i] = iter->value().ToString();
//                keysInProgress += 1;
                s = threadPool->addTask(&SepDB::readValue,
                                              this,
                                              std::ref(valueInfos[i]),
                                              &values[i]);
                iter->Next();
                if (!iter->Valid()) std::cerr << "not valid\n";
            }
            // wait for all threads to complete
            uint64_t wait = NowMiros();
            s.wait();
            STATS::time(STATS::getInstance()->waitScanThreadsFinish, wait, NowMiros());
            STATS::timeAndCount(STATS::getInstance()->scanVlogStat, startMicro, NowMiros());
            return i;
        }

        bool GetProperty(const string &property, std::string *value) {
            return indexDB->GetProperty(property, value);
        }


    private:
        DB *indexDB;
        FILE *vlog;
        ThreadPool *threadPool;
        VlogOptions rawOptions;

        // read value from vlog according to valueInfo read from index tree
        Status readValue(string &valueInfo, string *val) {
            size_t sepPos = valueInfo.find('$');
            string offsetStr = valueInfo.substr(0, sepPos);
            string valueSizeStr = valueInfo.substr(sepPos + 1, valueInfo.size() - sepPos + 1);
            long offset = std::stol(offsetStr);
            long valueSize = std::stol(valueSizeStr);
            char value[valueSize];
            pread(fileno(vlog), value, valueSize, offset);
            val->assign(value, valueSize);
            return Status();
        }
    };
}

#endif //MULTI_BF_LSM_VLOG_H