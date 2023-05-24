/**
 * @file test.c
 * Unit tests for the Apteryx XML Schema
 *
 * Copyright 2016, Allied Telesis Labs New Zealand, Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>
 */
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <assert.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <apteryx.h>

#define TEST_PATH           "/test"
#define TEST_ITERATIONS     1000
#define TEST_SCHEMA_PATH    "./models"

static inline uint64_t
get_time_us (void)
{
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (tv.tv_sec * (uint64_t) 1000000 + tv.tv_usec);
}

static bool
assert_apteryx_empty (void)
{
    GList *paths = apteryx_search ("/");
    GList *iter;
    bool ret = true;
    for (iter = paths; iter; iter = g_list_next (iter))
    {
        char *path = (char *) (iter->data);
        if (strncmp (TEST_PATH, path, strlen (TEST_PATH)) == 0)
        {
            if (ret)
                fprintf (stderr, "\n");
            fprintf (stderr, "ERROR: Node still set: %s\n", path);
            ret = false;
        }
    }
    g_list_free_full (paths, free);
    return ret;
}

static inline unsigned long
_memory_usage (void)
{
    unsigned long memory;
    FILE *f = fopen ("/proc/self/statm", "r");
    CU_ASSERT (1 == fscanf (f, "%*d %ld %*d %*d %*d %*d %*d", &memory)) fclose (f);
    return memory * getpagesize () / 1024;
}

static bool
_run_lua (char *script)
{
    char *buffer = strdup (script);
    lua_State *L;
    char *line;
    int res = -1;

    L = luaL_newstate ();
    luaL_openlibs (L);
    line = strtok (buffer, "\n");
    while (line != NULL)
    {
        res = luaL_loadstring (L, line);
        if (res == 0)
            res = lua_pcall (L, 0, 0, 0);
        if (res != 0)
            fprintf (stderr, "%s\n", lua_tostring (L, -1));
        CU_ASSERT (res == 0);
        line = strtok (NULL, "\n");
    }
    lua_close (L);
    free (buffer);
    return res == 0;
}

void
test_lua_lib_load (void)
{
    CU_ASSERT (_run_lua ("xml = require('apteryx.xml')"));
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_api_load (void)
{
    CU_ASSERT (_run_lua  ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')"));
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_api_set_get (void)
{
    char *value;
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api.test.settings.priority = '1'                                          \n"
                "assert(api.test.settings.priority == '1')                                 \n"));
    value = apteryx_get ("/test/settings/priority");
    CU_ASSERT (g_strcmp0 (value, "1") == 0);
    free (value);
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api.test.settings.priority = nil                                          \n"
                "assert(api.test.settings.priority == nil)                                 \n"));
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_ns_default_set_get (void)
{
    char *value;
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api['test:test'].settings.priority = '1'                                  \n"
                "assert(api['test:test'].settings.priority == '1')                         \n"));
    value = apteryx_get ("/test/settings/priority");
    CU_ASSERT (g_strcmp0 (value, "1") == 0);
    free (value);
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api['test:test'].settings.priority = nil                                  \n"
                "assert(api['test:test'].settings.priority == nil)                         \n"));
    CU_ASSERT (assert_apteryx_empty ());
}


void
test_lua_ns_other_set_get (void)
{
    char *value;
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api['t2:test'].settings.priority = '2'                                    \n"
                "assert(api['t2:test'].settings.priority == '2')                           \n"));
    value = apteryx_get ("/t2:test/settings/priority");
    CU_ASSERT (g_strcmp0 (value, "2") == 0);
    free (value);
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api['t2:test'].settings.priority = nil                                    \n"
                "assert(api['t2:test'].settings.priority == nil)                           \n"));
    CU_ASSERT (assert_apteryx_empty ());
}


void
test_lua_api_list (void)
{
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api.test.animals.animal('hamster').food('banana').type = 'fruit'          \n"
                "assert(api.test.animals.animal('hamster').food('banana').type == 'fruit') \n"
                "api.test.animals.animal('hamster').food('banana').type = nil              \n"
                "assert(api.test.animals.animal('hamster').food('banana').type == nil)     \n"));
    CU_ASSERT (assert_apteryx_empty ());
}


void
test_lua_api_trivial_list (void)
{
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api.test.animals.animal.parrot.toys.toy('puzzles', 'puzzles')             \n"
                "assert(api.test.animals.animal.parrot.toys.toy('puzzles') == 'puzzles')   \n"
                "api.test.animals.animal.parrot.toys.toy('puzzles', nil)                   \n"
                "assert(api.test.animals.animal.parrot.toys.toy('puzzles') == nil)         \n"));
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_api_search (void)
{
    CU_ASSERT (_run_lua
               ("api = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')                  \n"
                "api.test.animals.animal('cat').food('banana').name = 'banana'             \n"
                "api.test.animals.animal('cat').food('orange').name = 'orange'             \n"
                "api.test.animals.animal('cat').food('cabbage').name = 'cabbage'           \n"
                "api.test.animals.animal('dog').food('meat').name = 'meat'                 \n"
                "api.test.animals.animal('dog').food('frog').name = 'frog'                 \n"
                "cats = api.test.animals.animal('cat').food()                              \n"
                "assert(#cats == 3)                                                        \n"
                "dogs = api.test.animals.animal('dog').food()                              \n"
                "assert(#dogs == 2)                                                        \n"
                "api.test.animals.animal('cat').food('banana').name = nil                  \n"
                "api.test.animals.animal('cat').food('orange').name = nil                  \n"
                "api.test.animals.animal('cat').food('cabbage').name = nil                 \n"
                "api.test.animals.animal('dog').food('meat').name = nil                    \n"
                "api.test.animals.animal('dog').food('frog').name = nil                    \n"));
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_load_api_memory (void)
{
    lua_State *L;
    unsigned long before;
    unsigned long after;
    int res = -1;

    before = _memory_usage ();
    L = luaL_newstate ();
    luaL_openlibs (L);
    res =
        luaL_loadstring (L, "apteryx = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')");
    if (res == 0)
        res = lua_pcall (L, 0, 0, 0);
    if (res != 0)
        fprintf (stderr, "%s\n", lua_tostring (L, -1));
    after = _memory_usage ();
    lua_close (L);
    printf ("%ldkb ... ", (after - before));
    CU_ASSERT (res == 0);
}

void
test_lua_load_api_performance (void)
{
    uint64_t start;
    int i;

    start = get_time_us ();
    for (i = 0; i < 10; i++)
    {
        CU_ASSERT (_run_lua
                   ("apteryx = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')"));
    }
    printf ("%" PRIu64 "us ... ", (get_time_us () - start) / 10);
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_api_perf_get ()
{
    lua_State *L;
    uint64_t start;
    int i;

    for (i = 0; i < TEST_ITERATIONS; i++)
    {
        char *path = NULL;
        CU_ASSERT (asprintf (&path, TEST_PATH "/animals/animal/%d/name", i) > 0);
        apteryx_set (path, "private");
        free (path);
    }
    L = luaL_newstate ();
    luaL_openlibs (L);
    CU_ASSERT (luaL_loadstring
               (L, "apteryx = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')") == 0);
    CU_ASSERT (lua_pcall (L, 0, 0, 0) == 0);
    start = get_time_us ();
    for (i = 0; i < TEST_ITERATIONS; i++)
    {
        char *cmd = NULL;
        int res;
        CU_ASSERT (asprintf (&cmd, "assert(apteryx.test.animals.animal('%d').name == 'private')", i) >
                   0);
        res = luaL_loadstring (L, cmd);
        if (res == 0)
            res = lua_pcall (L, 0, 0, 0);
        if (res != 0)
            fprintf (stderr, "%s\n", lua_tostring (L, -1));
        if (res != 0)
            goto exit;
        free (cmd);
    }
    printf ("%" PRIu64 "us ... ", (get_time_us () - start) / TEST_ITERATIONS);
  exit:
    lua_close (L);
    for (i = 0; i < TEST_ITERATIONS; i++)
    {
        char *path = NULL;
        CU_ASSERT (asprintf (&path, TEST_PATH "/animals/animal/%d/name", i) > 0);
        CU_ASSERT (apteryx_set (path, NULL));
        free (path);
    }
    CU_ASSERT (assert_apteryx_empty ());
}

void
test_lua_api_perf_set ()
{
    lua_State *L;
    uint64_t start;
    int i;

    L = luaL_newstate ();
    luaL_openlibs (L);
    CU_ASSERT (luaL_loadstring
               (L, "apteryx = require('apteryx.xml').api('" TEST_SCHEMA_PATH "')") == 0);
    CU_ASSERT (lua_pcall (L, 0, 0, 0) == 0);
    start = get_time_us ();
    for (i = 0; i < TEST_ITERATIONS; i++)
    {
        char *cmd = NULL;
        int res;
        CU_ASSERT (asprintf (&cmd, "apteryx.test.animals.animal('%d').name = 'private'", i) > 0);
        res = luaL_loadstring (L, cmd);
        if (res == 0)
            res = lua_pcall (L, 0, 0, 0);
        if (res != 0)
            fprintf (stderr, "%s\n", lua_tostring (L, -1));
        if (res != 0)
            goto exit;
        free (cmd);
    }
    printf ("%" PRIu64 "us ... ", (get_time_us () - start) / TEST_ITERATIONS);
  exit:
    lua_close (L);
    for (i = 0; i < TEST_ITERATIONS; i++)
    {
        char *path = NULL;
        CU_ASSERT (asprintf (&path, TEST_PATH "/animals/animal/%d/name", i) > 0);
        CU_ASSERT (apteryx_set (path, NULL));
        free (path);
    }
    CU_ASSERT (assert_apteryx_empty ());
}

static int
suite_init (void)
{
    return 0;
}

static int
suite_clean (void)
{
    return 0;
}

CU_TestInfo tests_lua[] = {
    {"lua load module", test_lua_lib_load},
    {"lua load models", test_lua_api_load},
    {"lua api set get", test_lua_api_set_get},
    {"lua ns default set get", test_lua_ns_default_set_get},
    {"lua ns other set get", test_lua_ns_other_set_get},
    {"lua api list", test_lua_api_list},
    {"lua api trivial list", test_lua_api_trivial_list},
    {"lua api search", test_lua_api_search},
    {"lua load api memory usage", test_lua_load_api_memory},
    {"lua load api performance", test_lua_load_api_performance},
    {"lua api get performance", test_lua_api_perf_get},
    {"lua api set performance", test_lua_api_perf_set},
    CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suites[] = {
    {"LUA API", suite_init, suite_clean, NULL, NULL, tests_lua},
    CU_SUITE_INFO_NULL,
};

int
main (int argc, char *argv[])
{
    char *filter = NULL;
    int i = 0;

    /* Parse options */
    while ((i = getopt (argc, argv, "u::")) != -1)
    {
        switch (i)
        {
        case 'u':
            if (optarg && optarg[0] == '=')
            {
                memmove (optarg, optarg + 1, strlen (optarg));
            }
            filter = optarg;
            break;
        default:
            return 0;
        }
    }

    /* Initialize the CUnit test registry */
    if (CUE_SUCCESS != CU_initialize_registry ())
        return -1;
    assert (NULL != CU_get_registry ());
    assert (!CU_is_test_running ());

    /* Make some random numbers */
    srand (time (NULL));

    /* Add tests */
    CU_SuiteInfo *suite = &suites[0];
    while (suite && suite->pName)
    {
        /* Default to running all tests of a suite */
        bool all = true;
        if (filter && strstr (suite->pName, filter) != NULL)
            all = true;
        else if (filter)
            all = false;
        CU_pSuite pSuite =
            CU_add_suite (suite->pName, suite->pInitFunc, suite->pCleanupFunc);
        if (pSuite == NULL)
        {
            fprintf (stderr, "suite registration failed - %s\n", CU_get_error_msg ());
            exit (EXIT_FAILURE);
        }
        CU_TestInfo *test = &suite->pTests[0];
        while (test && test->pName)
        {
            if (all || (filter && strstr (test->pName, filter) != NULL))
            {
                if (CU_add_test (pSuite, test->pName, test->pTestFunc) == NULL)
                {
                    fprintf (stderr, "test registration failed - %s\n",
                             CU_get_error_msg ());
                    exit (EXIT_FAILURE);
                }
            }
            test++;
        }
        suite++;
    }

    /* Run all tests using the CUnit Basic interface */
    CU_basic_set_mode (CU_BRM_VERBOSE);
    CU_set_error_action (CUEA_IGNORE);
    CU_basic_run_tests ();
    CU_cleanup_registry ();
    return 0;
}
