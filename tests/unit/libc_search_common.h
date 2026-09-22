/* Included only by libc_search_test.c's portable host branch. Tree topology,
 * GNU hash capacity/errno, and null-root extensions are not common contracts. */
#include <unistd.h>

static int checks, failures;

static void check(const char *label, int condition)
{
    checks++;
    if (!condition) failures++;
    printf("%s: %s\n", label, condition ? "ok" : "FAIL");
}

static void note(const char *message)
{
    write(2, message, strlen(message));
}

static int compare_int(const void *left, const void *right)
{
    int a = *(const int *)left, b = *(const int *)right;
    return (a > b) - (a < b);
}

static int walked[16], walk_count;

static void collect_sorted(const void *node, VISIT visit, int depth)
{
    (void)depth;
    if (visit == leaf || visit == postorder) {
        if (walk_count < 16) walked[walk_count] = **(const int *const *)node;
        walk_count++;
    }
}

static void tree_cases(void)
{
    int keys[] = {7, 3, 11, 1, 5, 9, 13, 0, 2, 4, 6, 8, 10, 12, 14};
    void *root = NULL;
    for (int i = 0; i < 15; i++) {
        void *node = tsearch(&keys[i], &root, compare_int);
        check("tree inserts original key", node && *(int **)node == &keys[i]);
    }
    int duplicate = 7;
    void *node = tsearch(&duplicate, &root, compare_int);
    check("duplicate retains original key", node && *(int **)node == &keys[0]);
    for (int value = 0; value < 15; value++) {
        node = tfind(&value, &root, compare_int);
        check("tree lookup", node && **(int **)node == value);
    }
    int missing = 99;
    check("tree miss", tfind(&missing, &root, compare_int) == NULL);
    walk_count = 0;
    twalk(root, collect_sorted);
    int sorted = walk_count == 15;
    for (int i = 0; i < 15 && sorted; i++) sorted = walked[i] == i;
    check("walk sorted projection", sorted);

    /* Do not compare tdelete's parent pointer or its root special case. The
     * tree's resulting membership is meaningful on balanced and plain trees. */
    for (int value = 0; value < 15; value++) {
        tdelete(&value, &root, compare_int);
        check("deleted key absent", tfind(&value, &root, compare_int) == NULL);
    }
    check("drained tree is empty", root == NULL);
}

static void linear_cases(void)
{
    int values[8] = {4, 7, 2};
    size_t count = 3;
    int key = 7;
    check("linear find", lfind(&key, values, &count, sizeof(int), compare_int) == &values[1]);
    key = 9;
    check("linear miss keeps count", lfind(&key, values, &count, sizeof(int), compare_int) == NULL && count == 3);
    check("linear insert appends", lsearch(&key, values, &count, sizeof(int), compare_int) == &values[3] && count == 4 && values[3] == 9);
    check("linear duplicate keeps count", lsearch(&key, values, &count, sizeof(int), compare_int) == &values[3] && count == 4);
}

static void queue_cases(void)
{
    struct link { struct link *next, *previous; int value; } first = {0}, middle = {0}, last = {0};
    insque(&middle, &first);
    insque(&last, &middle);
    check("queue forward links", first.next == &middle && middle.next == &last && last.next == NULL);
    check("queue backward links", last.previous == &middle && middle.previous == &first);
    remque(&middle);
    check("queue removal joins neighbors", first.next == &last && last.previous == &first);
    remque(&last);
    check("queue drains", first.next == NULL);
}

static void hash_cases(void)
{
    char *key = malloc(6), *duplicate = malloc(6);
    if (!key || !duplicate) {
        free(key); free(duplicate);
        check("hash fixture allocation", 0);
        return;
    }
    memcpy(key, "alpha", 6);
    memcpy(duplicate, "alpha", 6);
    if (!hcreate(16)) {
        free(key); free(duplicate);
        check("hash create", 0);
        return;
    }
    ENTRY original = {key, (void *)111}, repeated = {duplicate, (void *)222};
    ENTRY *entry = hsearch(original, ENTER);
    check("hash insert", entry && entry->data == (void *)111);
    entry = hsearch(repeated, ENTER);
    check("hash duplicate keeps data", entry && entry->data == (void *)111);
    entry = hsearch(original, FIND);
    check("hash find", entry && strcmp(entry->key, "alpha") == 0);
    ENTRY missing = {"absent", NULL};
    check("hash miss", hsearch(missing, FIND) == NULL);
    hdestroy();
    /* Apple documents hdestroy freeing inserted keys; GNU and LogitOS leave
     * them with the caller. Old stack-key GNU fixtures therefore crash on
     * Darwin. This is cleanup for each real API, not a replacement oracle. */
#if !defined(__APPLE__) || defined(LIBC_OURS_HOST_TEST)
    free(key);
#endif
    free(duplicate);
}

#ifdef LIBC_OURS_HOST_TEST
static int contract_checks, contract_failures;
static unsigned destroyed;

static void contract(const char *label, int condition)
{
    contract_checks++;
    if (!condition) {
        contract_failures++;
        printf("FAIL search contract: %s\n", label);
    }
}

static void destroy_key(void *key)
{
    destroyed |= 1u << *(int *)key;
    free(key);
}

static void extension_contracts(void)
{
    void *root = NULL;
    for (int i = 0; i < 4; i++) {
        int *key = malloc(sizeof *key);
        if (!key) { contract("destroy fixture allocation", 0); break; }
        *key = i;
        if (!tsearch(key, &root, compare_int)) {
            free(key); contract("destroy fixture insertion", 0); break;
        }
    }
    tdestroy(root, destroy_key);
    contract("tdestroy calls callback for each key", destroyed == 15);
    unsigned before_null = destroyed;
    tdestroy(NULL, destroy_key);
    contract("tdestroy NULL does not call callback", destroyed == before_null);

    struct hsearch_data first = {0}, second = {0};
    int created = hcreate_r(4, &first) && hcreate_r(4, &second);
    contract("independent tables created", created);
    if (created) {
        ENTRY a = {"shared", (void *)11}, b = {"shared", (void *)22};
        ENTRY *found = NULL;
        contract("first table insert", hsearch_r(a, ENTER, &found, &first) && found && found->data == a.data);
        contract("second table insert", hsearch_r(b, ENTER, &found, &second) && found && found->data == b.data);
        contract("first table remains independent", hsearch_r(a, FIND, &found, &first) && found && found->data == a.data);
        ENTRY missing = {"missing", NULL};
        errno = 0;
        contract("reentrant miss reports ESRCH", !hsearch_r(missing, FIND, &found, &first) && found == NULL && errno == ESRCH);
    }
    hdestroy_r(&first);
    hdestroy_r(&second);
    contract("destroy clears table handles", first.table == NULL && second.table == NULL);
    contract("table can be recreated", hcreate_r(3, &first));
    ENTRY absent = {"shared", NULL}, *found = NULL;
    contract("recreated table has no stale key", !hsearch_r(absent, FIND, &found, &first));
    hdestroy_r(&first);
    char message[96];
    snprintf(message, sizeof message, "search project contracts: %d checks, %d failures\n", contract_checks, contract_failures);
    note(message);
}
#endif

int main(void)
{
    tree_cases();
    linear_cases();
    queue_cases();
    hash_cases();
    printf("search common: %d checks, %d failures\n", checks, failures);
#ifdef LIBC_OURS_HOST_TEST
    extension_contracts();
    failures += contract_failures;
#else
    note("SKIP GNU search reference: Darwin lacks tdestroy/hcreate_r/hsearch_r/hdestroy_r and has different hash capacity, errno and key ownership; run make test-libc-search on a glibc host for the original full differential.\n");
#endif
    return failures ? 1 : 0;
}
