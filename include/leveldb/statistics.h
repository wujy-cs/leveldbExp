//
// Created by wujy on 18-9-2.
//

#ifndef LEVELDB_STATISTICS_H
#define LEVELDB_STATISTICS_H

#include <string>
#include <vector>
#include <iostream>

#define STATS leveldb::Stats
#define NowMiros() Env::Default()->NowMicros()

namespace leveldb {
    using statType = std::pair<uint32_t, uint64_t>;   // <op counts, total time>

    //singleton statistics class
    class Stats {
    public:
        static Stats *GetInstance() {
            static Stats expStats;
            return &expStats;
        }

        statType writeStat;
        statType readStat;
        std::vector<statType> readLevelStat;
        statType readVlogStat;
        statType scanVlogStat;
        statType writeVlogStat;
        uint64_t waitFlush;
        uint64_t scanVlogIter;
        uint64_t writeMemtable;
        uint64_t waitScanThreadsFinish;
        uint64_t compactionIterTime;             //time of iteration during compaction
        uint64_t compactionOutputTime;           //time of writing output file during compaction
        uint64_t compactionAddToBuilder;         //time of adding kv to table builder
        uint64_t compactionFindNext;             //iterator to next
        uint64_t compactionSync;
        uint64_t assignThread;
        uint64_t gcTime;
        uint64_t gcPutBack;
        uint64_t gcReadLsm;
        uint64_t gcWritebackBytes;
        uint64_t addIndexBlock;
        uint64_t addFilter;
        uint64_t addData;
        uint64_t flushTable;
        uint64_t writeDiskSize;
        uint64_t vlogWriteDisk;
        uint64_t finishTable;
        uint64_t lsmIOTime;
        uint64_t generateFilterTime;
        uint64_t vtableWriteBuffer;

        static void TimeAndCount(statType &stat, uint64_t start, uint64_t end) {
            stat.first++;
            stat.second += (end - start);
        }

        static void Time(uint64_t &stat, uint64_t start, uint64_t end) {
            stat += (end - start);
        }

        static void Add(uint64_t &stat, uint64_t n) {
            stat += n;
        }

        void printAll() {
            printf("\n");
            printVlogRW();
            printRW();
            printf("\n");
        }

        // print lsm-tree read/write stats
        void printRW() {
            uint32_t writeCnt = writeStat.first;
            double writeTime = writeStat.second;
            uint32_t readCnt = readStat.first;
            double readTime = readStat.second;
            double memTime = writeMemtable;
            double cOutputTime = compactionOutputTime;
            double cIterTime = compactionIterTime;
            double cNextTime = compactionFindNext;
            double builderTime = compactionAddToBuilder;
            printf("LSM-Tree write time: %.2f s, count: %u, write latency: %.2f us\n", writeTime / 1000000, writeCnt,
                   writeTime / writeCnt);
            printf("LSM-Tree read time: %.2f s, count: %u, read latency: %.2f us\n", readTime / 1000000, readCnt,
                   readTime / readCnt);
            printf("LSM-Tree write memtable time: %.2f s\n", memTime / 1000000);
            printf("LSM-Tree wait imm flush time: %.2f s\n", waitFlush/1000000.0);
            for(int i = 0;i<readLevelStat.size();i++) {
                if(readLevelStat[i].first>0)
                printf("LSM-Tree read level %d file time: %.2f s, count: %u\n",i,readLevelStat[i].second/1000000.0, readLevelStat[i].first);
            }
            printf("LSM-Tree compactionIterTime: %.2f s\n", cIterTime / 1000000);
            printf("LSM-Tree compaction output file time: %.2f s\n", cOutputTime / 1000000);
            printf("LSM-Tree compaction sync time: %.2f\n",compactionSync/1000000.0);
            printf("LSM-Tree compaction add to builder time: %.2f s\n", builderTime / 1000000);
            printf("LSM-Tree compaction iterator find next kv time: %.2f s\n",cNextTime / 1000000);
            printf("LSM-Tree add index block time: %.2f\n",addIndexBlock/1000000.0);
            printf("LSM-Tree add key to filter builder time: %.2f\n",addFilter/1000000.0);
            printf("LSM-Tree generate filter time: %.2f\n",generateFilterTime/1000000.0);
            printf("LSM-Tree add data time: %.2f\n",addData/1000000.0);
            printf("LSM-Tree flush table time: %.2f\n",flushTable/1000000.0);
            printf("LSM-Tree finish table time: %.2f\n",finishTable/1000000.0);
            printf("LSM-Tree write disk size: %.2fMB\n",writeDiskSize/1000000.0);
            printf("LSM-Tree IO time: %.2f\n",lsmIOTime/1000000.0);
        }

        // print vlog/expdb read/write stats
        void printVlogRW() {
            uint32_t writeCnt = writeVlogStat.first;
            double writeTime = writeVlogStat.second;
            uint32_t readCnt = readVlogStat.first;
            double readTime = readVlogStat.second;
            uint32_t scanCnt = scanVlogStat.first;
            double scanTime = scanVlogStat.second;
            double iterTime = scanVlogIter;
            double assign = assignThread;
            double gc = gcTime;
            printf("Total vlogdb write time: %.2f s, count: %u, write latency: %.2f us\n", writeTime / 1000000,
                   writeCnt, writeTime / writeCnt);
            printf("Total vlogdb read time: %.2f s, count: %u, read latency: %.2f us\n", readTime / 1000000, readCnt,
                   readTime / readCnt);
            printf("Total vlogdb scan time:%.2f s, count: %u, scan latency: %.2f us\n", scanTime / 1000000, scanCnt,
                   scanTime / scanCnt);
            printf("Vtable write buffer time:%.2f s\n",vtableWriteBuffer/1000000.0);
            printf("Vlog scan LSM-Tree iter time: %.2f s\n", iterTime / 1000000);
            printf("Assign vlog thread time: %.2f s\n", assign / 1000000);
            printf("Wait vlog scan threads finish time: %.2f s\n", (double) waitScanThreadsFinish / 1000000);
            printf("Vlog gc time: %.2f s\n", (double) STATS::GetInstance()->gcTime / 1000000);
            printf("Read LSM time during gc: %.2f\n", (double) STATS::GetInstance()->gcReadLsm / 1000000);
            printf("Put back time during gc: %.2f\n", (double) STATS::GetInstance()->gcPutBack / 1000000);
            printf("Write back to vlog bytes during gc: %lu\n", gcWritebackBytes);
            printf("vlog write disk size: %.2fMB\n",vlogWriteDisk/1000000.0);
        }


    private:

        Stats() :readLevelStat(std::vector<statType>(10)) {
        }

    };
}

#endif //LEVELDB_STATISTICS_H
