/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "libforestdb/forestdb.h"
#include "hbtrie.h"
#include "test.h"
#include "btreeblock.h"
#include "docio.h"
#include "filemgr.h"
#include "filemgr_ops.h"

void _set_random_string(char *str, int len)
{
    str[len--] = 0;
    do {
        str[len] = '!' + random('~'-'!');
    } while(len--);
}

void _set_random_string_smallabt(char *str, int len)
{
    str[len--] = 0;
    do {
        str[len] = 'a' + random('z'-'a');
    } while(len--);
}

void basic_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 10;
    fdb_handle db;
    fdb_handle db_rdonly;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 0 * 1024 * 1024;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // Read-Only mode test: Must not create new file..
    config.durability_opt = FDB_DRB_RDONLY;
    status = fdb_open(&db, (char *) "./dummy1", &config);
    TEST_CHK(status == FDB_RESULT_FAIL);
    config.durability_opt = 0;

    // open and close db
    fdb_open(&db, "./dummy1", &config);
    fdb_close(&db);

    // reopen db
    fdb_open(&db, "./dummy1", &config);

    // insert documents
    for (i=0;i<n;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // remove document #5
    fdb_doc_create(&rdoc, doc[5]->key, doc[5]->keylen, doc[5]->meta, doc[5]->metalen, NULL, 0);
    fdb_set(&db, rdoc);
    fdb_doc_free(rdoc);

    // commit
    fdb_commit(&db);

    // close the db
    fdb_close(&db);

    // reopen
    fdb_open(&db, "./dummy1", &config);

    // update document #0 and #1
    for (i=0;i<2;++i){
        sprintf(metabuf, "meta2%d", i);
        sprintf(bodybuf, "body2%d", i);
        fdb_doc_update(&doc[i], (void *)metabuf, strlen(metabuf),
            (void *)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // commit
    fdb_commit(&db);

    // retrieve documents
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        if (i != 5) {
            // updated documents
            TEST_CHK(status == FDB_RESULT_SUCCESS);
            TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
            TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));
        } else {
            // removed document
            TEST_CHK(status == FDB_RESULT_FAIL);
        }

        // free result document
        fdb_doc_free(rdoc);
    }

    // do compaction
    fdb_compact(&db, (char *) "./dummy2");

    // retrieve documents after compaction
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        if (i != 5) {
            // updated documents
            TEST_CHK(status == FDB_RESULT_SUCCESS);
            TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
            TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));
        } else {
            // removed document
            TEST_CHK(status == FDB_RESULT_FAIL);
        }

        // free result document
        fdb_doc_free(rdoc);
    }

    // retrieve documents by sequence number
    for (i=0;i<n;++i){
        // search by seq
        fdb_doc_create(&rdoc, NULL, 0, NULL, 0, NULL, 0);
        rdoc->seqnum = i;
        status = fdb_get_byseq(&db, rdoc);

        if ( (i>=2 && i<=4) || (i>=6 && i<=9) || (i>=11 && i<=12)) {
            // updated documents
            TEST_CHK(status == FDB_RESULT_SUCCESS);
        } else {
            // removed document
            TEST_CHK(status == FDB_RESULT_FAIL);
        }

        // free result document
        fdb_doc_free(rdoc);
    }

    // Read-Only mode test: Open succeeds if file exists, but disallow writes
    config.durability_opt = FDB_DRB_RDONLY;
    status = fdb_open(&db_rdonly, (char *) "./dummy2", &config);
    TEST_CHK(status == FDB_RESULT_SUCCESS);

    fdb_doc_create(&rdoc, doc[0]->key, doc[0]->keylen, NULL, 0, NULL, 0);
    status = fdb_get(&db_rdonly, rdoc);
    TEST_CHK(status == FDB_RESULT_SUCCESS);

    status = fdb_set(&db_rdonly, doc[i]);
    TEST_CHK(status == FDB_RESULT_FAIL);

    status = fdb_commit(&db_rdonly);
    TEST_CHK(status == FDB_RESULT_FAIL);

    status = fdb_flush_wal(&db_rdonly);
    TEST_CHK(status == FDB_RESULT_FAIL);

    fdb_doc_free(rdoc);
    fdb_close(&db_rdonly);

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // do one more compaction
    fdb_compact(&db, (char *) "./dummy3");

    // close db file
    fdb_close(&db);

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("basic test");
}

void wal_commit_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 10;
    fdb_handle db;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 0 * 1024 * 1024;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);

    // insert half documents
    for (i=0;i<n/2;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void *)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // commit
    fdb_commit(&db);

    // insert the other half documents
    for (i=n/2;i<n;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void *)keybuf, strlen(keybuf),
            (void *)metabuf, strlen(metabuf), (void *)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // close the db
    fdb_close(&db);

    // reopen
    fdb_open(&db, "./dummy1", &config);

    // retrieve documents
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        if (i < n/2) {
            // committed documents
            TEST_CHK(status == FDB_RESULT_SUCCESS);
            TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
            TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));
        } else {
            // not committed document
            TEST_CHK(status == FDB_RESULT_FAIL);
        }

        // free result document
        fdb_doc_free(rdoc);
    }

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // close db file
    fdb_close(&db);

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("WAL commit test");
}

void multi_version_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 2;
    fdb_handle db, db_new;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 1 * 1024 * 1024;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);

    // insert documents
    for (i=0;i<n;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // manually flush WAL
    fdb_flush_wal(&db);
    // commit
    fdb_commit(&db);

    // open same db file using a new handle
    fdb_open(&db_new, "./dummy1", &config);

    // update documents using the old handle
    for (i=0;i<n;++i){
        sprintf(metabuf, "meta2%d", i);
        sprintf(bodybuf, "body2%d", i);
        fdb_doc_update(&doc[i], (void*)metabuf, strlen(metabuf),
            (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // manually flush WAL and commit using the old handle
    fdb_flush_wal(&db);
    fdb_commit(&db);

    // retrieve documents using the old handle
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        TEST_CHK(status == FDB_RESULT_SUCCESS);
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        // free result document
        fdb_doc_free(rdoc);
    }

    // retrieve documents using the new handle
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db_new, rdoc);

        TEST_CHK(status == FDB_RESULT_SUCCESS);
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        // free result document
        fdb_doc_free(rdoc);
    }

    // close and re-open the new handle
    fdb_close(&db_new);
    fdb_open(&db_new, "./dummy1", &config);

    // retrieve documents using the new handle
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db_new, rdoc);

        TEST_CHK(status == FDB_RESULT_SUCCESS);
        // the new version of data should be read
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        // free result document
        fdb_doc_free(rdoc);
    }


    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // close db file
    fdb_close(&db);
    fdb_close(&db_new);

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("multi version test");
}

void compact_wo_reopen_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 3;
    fdb_handle db, db_new;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 1 * 1024 * 1024;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);
    fdb_open(&db_new, "./dummy1", &config);

    // insert documents
    for (i=0;i<n;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // remove doc
    fdb_doc_create(&rdoc, doc[1]->key, doc[1]->keylen, doc[1]->meta, doc[1]->metalen, NULL, 0);
    fdb_set(&db, rdoc);
    fdb_doc_free(rdoc);

    // manually flush WAL
    fdb_flush_wal(&db);
    // commit
    fdb_commit(&db);

    // perform compaction using one handle
    fdb_compact(&db, (char *) "./dummy2");

    // retrieve documents using the other handle without close/re-open
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db_new, rdoc);

        if (i != 1) {
            TEST_CHK(status == FDB_RESULT_SUCCESS);
            TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
            TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));
        }else{
            TEST_CHK(status == FDB_RESULT_FAIL);
        }

        // free result document
        fdb_doc_free(rdoc);
    }
    // check the other handle's filename
    TEST_CHK(!strcmp("./dummy2", db_new.file->filename));

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // close db file
    fdb_close(&db);
    fdb_close(&db_new);

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("compaction without reopen test");
}

void auto_recover_compact_ok_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 3;
    fdb_handle db, db_new;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc *, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 1 * 1024 * 1024;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL " dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);
    fdb_open(&db_new, "./dummy1", &config);

    // insert first two documents
    for (i=0;i<2;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // remove second doc
    fdb_doc_create(&rdoc, doc[1]->key, doc[1]->keylen, doc[1]->meta, doc[1]->metalen, NULL, 0);
    fdb_set(&db, rdoc);
    fdb_doc_free(rdoc);

    // manually flush WAL
    fdb_flush_wal(&db);
    // commit
    fdb_commit(&db);

    // perform compaction using one handle
    fdb_compact(&db, (char *) "./dummy2");

    // save the old file after compaction is done ..
    r = system(SHELL_COPY " dummy1 dummy11 > errorlog.txt");

    // now insert third doc: it should go to the newly compacted file.
    sprintf(keybuf, "key%d", i);
    sprintf(metabuf, "meta%d", i);
    sprintf(bodybuf, "body%d", i);
    fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
        (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
    fdb_set(&db, doc[i]);

    // manually flush WAL
    fdb_flush_wal(&db);
    // commit
    fdb_commit(&db);

    // close both the db files ...
    fdb_close(&db);
    fdb_close(&db_new);

    // restore the old file after close is done ..
    r = system(SHELL_MOVE " dummy11 dummy1 > errorlog.txt");

    // now open the old saved compacted file, it should automatically recover
    // and use the new file since compaction was done successfully
    fdb_open(&db_new, "./dummy1", &config);

    // retrieve documents using the old handle and expect all 3 docs
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db_new, rdoc);

        if (i != 1) {
            TEST_CHK(status == FDB_RESULT_SUCCESS);
            TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
            TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));
        }else{
            TEST_CHK(status == FDB_RESULT_FAIL);
        }

        // free result document
        fdb_doc_free(rdoc);
    }
    // check this handle's filename it should point to newly compacted file
    TEST_CHK(!strcmp("./dummy2", db_new.file->filename));

    // close the file
    fdb_close(&db_new);

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("auto recovery after compaction test");
}

struct work_thread_args{
    int tid;
    size_t ndocs;
    size_t writer;
    fdb_config *config;
    fdb_doc **doc;
    size_t time_sec;
    size_t nbatch;
    size_t compact_term;
    int *filename_count;
    spin_t *filename_count_lock;
};

//#define FILENAME "./hdd/dummy"
#define FILENAME "dummy"

#define KSIZE (100)
#define VSIZE (100)
#define IDX_DIGIT (7)
#define IDX_DIGIT_STR "7"

void *_worker_thread(void *voidargs)
{
    struct work_thread_args *args = (struct work_thread_args *)voidargs;
    int i, r, k, c, commit_count, filename_count;
    struct timeval ts_begin, ts_cur, ts_gap;
    fdb_handle db;
    fdb_status status;
    fdb_doc *rdoc;
    char temp[1024];

    char cnt_str[IDX_DIGIT+1];
    int cnt_int;

    filename_count = *args->filename_count;
    sprintf(temp, FILENAME"%d", filename_count);
    fdb_open(&db, temp, args->config);

    gettimeofday(&ts_begin, NULL);

    c = cnt_int = commit_count = 0;
    cnt_str[IDX_DIGIT] = 0;

    while(1){
        i = rand() % args->ndocs;
        fdb_doc_create(&rdoc, args->doc[i]->key, args->doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        assert(status == FDB_RESULT_SUCCESS);
        assert(!memcmp(rdoc->body, args->doc[i]->body, (IDX_DIGIT+1)));

        if (args->writer) {
            // if writer,
            // copy and parse the counter in body
            memcpy(cnt_str, (uint8_t *)rdoc->body + (IDX_DIGIT+1), IDX_DIGIT);
            cnt_int = atoi(cnt_str);

            // increase and rephrase
            sprintf(cnt_str, "%0"IDX_DIGIT_STR"d", ++cnt_int);
            memcpy((uint8_t *)rdoc->body + (IDX_DIGIT+1), cnt_str, IDX_DIGIT);

            // update and commit
            status = fdb_set(&db, rdoc);

            if (args->nbatch > 0) {
                if (c % args->nbatch == 0) {
                    // commit for every NBATCH
                    fdb_commit(&db);
                    commit_count++;

                    if (args->compact_term == commit_count &&
                        args->compact_term > 0 &&
                        db.new_file == NULL) {
                        // do compaction for every COMPACT_TERM batch
                        spin_lock(args->filename_count_lock);
                        *args->filename_count += 1;
                        filename_count = *args->filename_count;
                        spin_unlock(args->filename_count_lock);

                        sprintf(temp, FILENAME"%d", filename_count);

                        status = fdb_compact(&db, temp);

                        commit_count = 0;
                    }
                }
            }
        }
        fdb_doc_free(rdoc);
        c++;

        gettimeofday(&ts_cur, NULL);
        ts_gap = _utime_gap(ts_begin, ts_cur);
        if (ts_gap.tv_sec >= args->time_sec) break;
    }

    DBG("Thread #%d (%s) %d ops / %d seconds\n",
        args->tid, (args->writer)?("writer"):("reader"), c, (int)args->time_sec);

    fdb_flush_wal(&db);
    fdb_commit(&db);

    fdb_close(&db);
    thread_exit(0);
    return NULL;
}

void multi_thread_test(
    size_t ndocs, size_t wal_threshold, size_t time_sec,
    size_t nbatch, size_t compact_term, size_t nwriters, size_t nreaders)
{
    TEST_INIT();

    int i, r, idx_digit, temp_len;
    int n = nwriters + nreaders;;
    thread_t *tid = alca(thread_t, n);
    void **thread_ret = alca(void *, n);
    struct work_thread_args *args = alca(struct work_thread_args, n);
    struct timeval ts_begin, ts_cur, ts_gap;
    fdb_handle db, db_new;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, ndocs);
    fdb_doc *rdoc;
    fdb_status status;

    int filename_count = 1;
    spin_t filename_count_lock;
    spin_init(&filename_count_lock);

    char keybuf[1024], metabuf[1024], bodybuf[1024], temp[1024];

    idx_digit = IDX_DIGIT;

    // remove previous dummy files
    r = system(SHELL_DEL" "FILENAME"* > errorlog.txt");

    memleak_start();

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = (uint64_t)16 * 1024 * 1024;
    config.wal_threshold = wal_threshold;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;
    config.durability_opt = FDB_DRB_NONE;

    // initial population ===
    DBG("Initialize..\n");

    // open db
    sprintf(temp, FILENAME"%d", filename_count);
    fdb_open(&db, temp, &config);

    gettimeofday(&ts_begin, NULL);

    // insert documents
    for (i=0;i<ndocs;++i){
        _set_random_string_smallabt(temp, KSIZE - (IDX_DIGIT+1));
        sprintf(keybuf, "k%0"IDX_DIGIT_STR"d%s", i, temp);

        sprintf(metabuf, "m%0"IDX_DIGIT_STR"d", i);

        _set_random_string_smallabt(temp, VSIZE-(IDX_DIGIT*2+1));
        sprintf(bodybuf, "b%0"IDX_DIGIT_STR"d%0"IDX_DIGIT_STR"d%s", i, 0, temp);

        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    fdb_flush_wal(&db);
    fdb_commit(&db);

    gettimeofday(&ts_cur, NULL);
    ts_gap = _utime_gap(ts_begin, ts_cur);
    //DBG("%d.%09d seconds elapsed\n", (int)ts_gap.tv_sec, (int)ts_gap.tv_nsec);

    fdb_close(&db);
    // end of population ===

    // drop OS's page cache
    //r = system("free && sync && echo 3 > /proc/sys/vm/drop_caches && free");

    // create workers
    for (i=0;i<n;++i){
        args[i].tid = i;
        args[i].writer = ((i<nwriters)?(1):(0));
        args[i].ndocs = ndocs;
        args[i].config = &config;
        args[i].doc = doc;
        args[i].time_sec = time_sec;
        args[i].nbatch = nbatch;
        args[i].compact_term = compact_term;
        args[i].filename_count = &filename_count;
        args[i].filename_count_lock = &filename_count_lock;
        thread_create(&tid[i], _worker_thread, &args[i]);
    }

    fprintf(stderr, "wait for %d seconds..\n", (int)time_sec);

    // wait for thread termination
    for (i=0;i<n;++i){
        thread_join(tid[i], &thread_ret[i]);
    }

    // free all documents
    for (i=0;i<ndocs;++i){
        fdb_doc_free(doc[i]);
    }

    fprintf(stderr, "compaction recovery..\n");
    sprintf(temp, FILENAME"_recovered");
    fdb_open(&db, temp, &config);
    sprintf(temp, FILENAME"%d", filename_count);
    // test _fdb_recover_compaction(&db, temp);
    fdb_close(&db);

    // shutdown
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("multi thread test");
}

void crash_recovery_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 10;
    fdb_handle db;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 0 * 1024 * 1024;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // reopen db
    fdb_open(&db, "./dummy2", &config);

    // insert documents
    for (i=0;i<n;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // commit
    fdb_commit(&db);

    // close the db
    fdb_close(&db);

    // Shutdown forest db in the middle of the test to simulate crash
    fdb_shutdown();

    // Now append garbage at the end of the file for a few blocks
    r = system(
       "dd if=/dev/zero bs=4096 of=./dummy2 oseek=3 count=2 >> errorlog.txt");

    // reopen the same file
    fdb_open(&db, "./dummy2", &config);

    // retrieve documents
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        TEST_CHK(status == FDB_RESULT_SUCCESS);
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        // free result document
        fdb_doc_free(rdoc);
    }

    // retrieve documents by sequence number
    for (i=0;i<n;++i){
        // search by seq
        fdb_doc_create(&rdoc, NULL, 0, NULL, 0, NULL, 0);
        rdoc->seqnum = i;
        status = fdb_get_byseq(&db, rdoc);

        TEST_CHK(status == FDB_RESULT_SUCCESS);

        // free result document
        fdb_doc_free(rdoc);
    }

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // close db file
    fdb_close(&db);

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("crash recovery test");
}

void incomplete_block_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 2;
    fdb_handle db;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 0;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);

    // insert documents
    for (i=0;i<n;++i){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }

    // retrieve documents
    for (i=0;i<n;++i){
        // search by key
        fdb_doc_create(&rdoc, doc[i]->key, doc[i]->keylen, NULL, 0, NULL, 0);
        status = fdb_get(&db, rdoc);

        // updated documents
        TEST_CHK(status == FDB_RESULT_SUCCESS);
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        // free result document
        fdb_doc_free(rdoc);
    }

    // close db file
    fdb_close(&db);

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("incomplete block test");
}

void iterator_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 10;
    uint64_t offset;
    fdb_handle db;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;
    fdb_iterator iterator;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 0;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);

    // insert documents of even number
    for (i=0;i<n;i+=2){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }
    // manually flush WAL & commit
    fdb_flush_wal(&db);
    fdb_commit(&db);

    // insert documents of odd number
    for (i=1;i<n;i+=2){
        sprintf(keybuf, "key%d", i);
        sprintf(metabuf, "meta%d", i);
        sprintf(bodybuf, "body%d", i);
        fdb_doc_create(&doc[i], (void*)keybuf, strlen(keybuf),
            (void*)metabuf, strlen(metabuf), (void*)bodybuf, strlen(bodybuf));
        fdb_set(&db, doc[i]);
    }
    // commit without WAL flush
    fdb_commit(&db);

    // now even number docs are in hb-trie & odd number docs are in WAL

    // create an iterator for full range
    fdb_iterator_init(&db, &iterator, NULL, 0, NULL, 0, FDB_ITR_NONE);

    // repeat until fail
    i=0;
    while(1){
        status = fdb_iterator_next(&iterator, &rdoc);
        if (status == FDB_RESULT_FAIL) break;

        TEST_CHK(!memcmp(rdoc->key, doc[i]->key, rdoc->keylen));
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        fdb_doc_free(rdoc);
        i++;
    };
    TEST_CHK(i==10);
    fdb_iterator_close(&iterator);

    // create an iterator with metaonly option
    fdb_iterator_init(&db, &iterator, NULL, 0, NULL, 0, FDB_ITR_METAONLY);

    // repeat until fail
    i=0;
    while(1){
        // retrieve the next doc and get the byte offset of the returned doc
        offset = BLK_NOT_FOUND;
        status = fdb_iterator_next_offset(&iterator, &rdoc, &offset);
        if (status == FDB_RESULT_FAIL) break;

        TEST_CHK(offset != BLK_NOT_FOUND);
        TEST_CHK(!memcmp(rdoc->key, doc[i]->key, rdoc->keylen));
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(rdoc->body == NULL);

        fdb_doc_free(rdoc);
        i++;
    };
    TEST_CHK(i==10);
    fdb_iterator_close(&iterator);

    // create another iterator starts from doc[3]
    sprintf(keybuf, "key%d", 3);
    fdb_iterator_init(&db, &iterator, (void*)keybuf, strlen(keybuf), NULL, 0, FDB_ITR_NONE);

    // repeat until fail
    i=3;
    while(1){
        status = fdb_iterator_next(&iterator, &rdoc);
        if (status == FDB_RESULT_FAIL) break;

        TEST_CHK(!memcmp(rdoc->key, doc[i]->key, rdoc->keylen));
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        fdb_doc_free(rdoc);
        i++;
    };
    TEST_CHK(i==10);
    fdb_iterator_close(&iterator);

    // create another iterator for the range of doc[4] ~ doc[8]
    sprintf(keybuf, "key%d", 4);
    sprintf(temp, "key%d", 8);
    fdb_iterator_init(&db, &iterator, (void*)keybuf, strlen(keybuf),
        (void*)temp, strlen(temp), FDB_ITR_NONE);

    // repeat until fail
    i=4;
    while(1){
        status = fdb_iterator_next(&iterator, &rdoc);
        if (status == FDB_RESULT_FAIL) break;

        TEST_CHK(!memcmp(rdoc->key, doc[i]->key, rdoc->keylen));
        TEST_CHK(!memcmp(rdoc->meta, doc[i]->meta, rdoc->metalen));
        TEST_CHK(!memcmp(rdoc->body, doc[i]->body, rdoc->bodylen));

        fdb_doc_free(rdoc);
        i++;
    };
    TEST_CHK(i==9);
    fdb_iterator_close(&iterator);

    // close db file
    fdb_close(&db);

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("iterator test");
}

int _cmp_double(void *a, void *b)
{
    double aa, bb;
    aa = *(double *)a;
    bb = *(double *)b;

    if (aa<bb) {
        return -1;
    } else if (aa>bb) {
        return 1;
    } else {
        return 0;
    }
}

void custom_compare_test()
{
    TEST_INIT();

    memleak_start();

    int i, r;
    int n = 10;
    uint64_t offset;
    fdb_handle db;
    fdb_config config;
    fdb_doc **doc = alca(fdb_doc*, n);
    fdb_doc *rdoc;
    fdb_status status;
    fdb_iterator iterator;

    char keybuf[256], metabuf[256], bodybuf[256], temp[256];
    double key_double, key_double_prev;

    // configuration
    memset(&config, 0, sizeof(fdb_config));
    config.chunksize = config.offsetsize = sizeof(uint64_t);
    config.buffercache_size = 0;
    config.wal_threshold = 1024;
    config.seqtree_opt = FDB_SEQTREE_USE;
    config.flag = 0;

    // remove previous dummy files
    r = system(SHELL_DEL" dummy* > errorlog.txt");

    // open db
    fdb_open(&db, "./dummy1", &config);

    // set custom compare function for double key type
    fdb_set_custom_cmp(&db, _cmp_double);

    for (i=0;i<n;++i){
        key_double = 10000/(i*11.0);
        memcpy(keybuf, &key_double, sizeof(key_double));
        sprintf(bodybuf, "value: %d, %f", i, key_double);
        fdb_doc_create(&doc[i], (void*)keybuf, sizeof(key_double), NULL, 0,
            (void*)bodybuf, strlen(bodybuf)+1);
        fdb_set(&db, doc[i]);
    }

    // range scan (before flushing WAL)
    fdb_iterator_init(&db, &iterator, NULL, 0, NULL, 0, 0x0);
    key_double_prev = -1;
    while(1){
        if ( (status = fdb_iterator_next(&iterator, &rdoc)) == FDB_RESULT_FAIL)
            break;
        memcpy(&key_double, rdoc->key, rdoc->keylen);
        TEST_CHK(key_double > key_double_prev);
        key_double_prev = key_double;
        fdb_doc_free(rdoc);
    };
    fdb_iterator_close(&iterator);

    fdb_flush_wal(&db);
    fdb_commit(&db);

    // range scan (after flushing WAL)
    fdb_iterator_init(&db, &iterator, NULL, 0, NULL, 0, 0x0);
    key_double_prev = -1;
    while(1){
        if ( (status = fdb_iterator_next(&iterator, &rdoc)) == FDB_RESULT_FAIL)
            break;
        memcpy(&key_double, rdoc->key, rdoc->keylen);
        TEST_CHK(key_double > key_double_prev);
        key_double_prev = key_double;
        fdb_doc_free(rdoc);
    };
    fdb_iterator_close(&iterator);

    // do compaction
    fdb_compact(&db, (char *) "./dummy2");

    // range scan (after compaction)
    fdb_iterator_init(&db, &iterator, NULL, 0, NULL, 0, 0x0);
    key_double_prev = -1;
    while(1){
        if ( (status = fdb_iterator_next(&iterator, &rdoc)) == FDB_RESULT_FAIL)
            break;
        memcpy(&key_double, rdoc->key, rdoc->keylen);
        TEST_CHK(key_double > key_double_prev);
        key_double_prev = key_double;
        fdb_doc_free(rdoc);
    };
    fdb_iterator_close(&iterator);

    // close db file
    fdb_close(&db);

    // free all documents
    for (i=0;i<n;++i){
        fdb_doc_free(doc[i]);
    }

    // free all resources
    fdb_shutdown();

    memleak_end();

    TEST_RESULT("custom compare test");
}


int main(){
    basic_test();
    wal_commit_test();
    multi_version_test();
    compact_wo_reopen_test();
    auto_recover_compact_ok_test();
#ifdef __CRC32
    crash_recovery_test();
#endif
    incomplete_block_test();
    iterator_test();
    custom_compare_test();
    multi_thread_test(40*1024, 1024, 20, 1, 100, 2, 6);

    return 0;
}