/* Unit tests: transactional save data (commit snapshot, rollback of uncommitted work, interrupted commit, pre-journal adoption). */
#include "../src/services/fs.c"
#include "check.h"
#include <ftw.h>

static char base[1024];
static void put(const char *dir, const char *name, const char *text) { char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "wb"); fwrite(text, 1, strlen(text), f); fclose(f); }
static int has(const char *dir, const char *name, const char *expect) {
    char p[1400], buf[256] = {0}; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0; return expect ? !strcmp(buf, expect) : 1;
}
/* "restart": forget the in-process journals so the next journal_get recovers from disk like a new process */
static void restart(void) { for (int i = 0; i < 16; i++) { free(g_journals[i]); g_journals[i] = NULL; } }

int main(void) {
    snprintf(base, sizeof base, "/tmp/napple-fsj-XXXXXX"); mkdtemp(base);
    char live[1100]; snprintf(live, sizeof live, "%s/account/4e617070", base);

    /* fresh save: write, commit, write more without committing, "crash" */
    SaveJournal *j = journal_get(live);
    CHECK(j && path_exists(live) && path_exists(j->marker) && !path_exists(j->committed), "fresh root: live dir and marker, no snapshot yet");
    put(live, "slot0.bin", "A1"); journal_mark_dirty(j);
    CHECK(journal_commit(j) == 0 && path_exists(j->committed) && has(j->committed, "slot0.bin", "A1") && !j->dirty && j->commits == 1, "commit snapshots the live state");
    put(live, "slot0.bin", "A2-partial"); put(live, "slot1.bin", "B1"); journal_mark_dirty(j);
    CHECK(has(live, "slot0.bin", "A2-partial") && has(j->committed, "slot0.bin", "A1") && !has(j->committed, "slot1.bin", NULL), "uncommitted writes stay out of the snapshot");
    restart(); j = journal_get(live);
    CHECK(has(live, "slot0.bin", "A1") && !has(live, "slot1.bin", NULL), "restart rolls back the uncommitted writes");

    /* a commit of an unchanged root does nothing (no snapshot churn) and still counts */
    { unsigned c0 = j->commits; CHECK(journal_commit(j) == 0 && j->commits == c0 + 1 && has(j->committed, "slot0.bin", "A1"), "clean commit is a no-op"); }

    /* a commit that completes survives a restart */
    put(live, "slot1.bin", "B1"); put(live, "slot0.bin", "A3"); journal_mark_dirty(j); CHECK(journal_commit(j) == 0, "second commit");
    restart(); j = journal_get(live);
    CHECK(has(live, "slot0.bin", "A3") && has(live, "slot1.bin", "B1"), "committed state survives a restart");

    /* a file deleted before the commit is gone afterwards; deleted after the last commit comes back */
    { char p[1400]; snprintf(p, sizeof p, "%s/slot1.bin", live); unlink(p); }
    restart(); j = journal_get(live); CHECK(has(live, "slot1.bin", "B1"), "uncommitted delete rolled back");
    { char p[1400]; snprintf(p, sizeof p, "%s/slot1.bin", live); unlink(p); } journal_mark_dirty(j); CHECK(journal_commit(j) == 0, "commit the delete");
    restart(); j = journal_get(live); CHECK(!has(live, "slot1.bin", NULL), "committed delete persists");

    /* crash between "committed -> previous" and "committing -> committed": previous is restored */
    rename(j->committed, j->old); put(live, "slot0.bin", "half-written"); mkdirs(j->tmp); put(j->tmp, "slot0.bin", "tmp");
    restart(); j = journal_get(live);
    CHECK(has(live, "slot0.bin", "A3") && !path_exists(j->old) && !path_exists(j->tmp) && path_exists(j->committed), "interrupted commit recovers the last complete snapshot");

    /* nested directories are part of the snapshot */
    { char d[1400]; snprintf(d, sizeof d, "%s/sub/deeper", live); mkdirs(d); put(d, "f.bin", "deep"); } journal_mark_dirty(j); CHECK(journal_commit(j) == 0, "commit with nested dirs");
    restart(); j = journal_get(live); { char d[1400]; snprintf(d, sizeof d, "%s/sub/deeper", live); CHECK(has(d, "f.bin", "deep"), "nested files restored"); }

    /* a save from before the journal existed (directory without marker) is adopted as the first committed state */
    char legacy[1100]; snprintf(legacy, sizeof legacy, "%s/account/aaaaaaaa", base); mkdirs(legacy); put(legacy, "old.bin", "OLD");
    SaveJournal *lj = journal_get(legacy);
    CHECK(has(legacy, "old.bin", "OLD") && has(lj->committed, "old.bin", "OLD") && path_exists(lj->marker), "pre-journal save adopted");
    /* marker present but never committed: nothing survives a crash */
    char fresh[1100]; snprintf(fresh, sizeof fresh, "%s/account/bbbbbbbb", base); SaveJournal *fj = journal_get(fresh); put(fresh, "x.bin", "X"); journal_mark_dirty(fj);
    restart(); fj = journal_get(fresh); CHECK(!has(fresh, "x.bin", NULL), "first-ever save never committed: discarded"); j = journal_get(live);
    /* open file descriptors keep working across a commit (the live directory is never swapped) */
    char fp[1400]; snprintf(fp, sizeof fp, "%s/open.bin", live); int fd = open(fp, O_CREAT | O_RDWR, 0644); (void)write(fd, "123", 3); journal_mark_dirty(j); /* IFile::Write marks the root dirty */
    CHECK(journal_commit(j) == 0, "commit with an open file"); (void)pwrite(fd, "456", 3, 3); close(fd);
    CHECK(has(live, "open.bin", "123456") && has(j->committed, "open.bin", "123"), "writes after the commit through an open fd stay uncommitted");
    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf %s", base); (void)system(cmd);
    printf("{\"test\":\"fs_journal\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
