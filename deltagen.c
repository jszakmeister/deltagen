/*
 Copyright (c) 2006-2009 by John Szakmeister <john at szakmeister dot net>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "svn_cmdline.h"
#include "svn_client.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_io.h"
#include <apr_lib.h>
#include <apr_file_io.h>
#include <stdio.h>
#include <string.h>

static void handle_error(svn_error_t *err, apr_pool_t *pool)
{
  if (err)
    svn_handle_error2(err, stderr, FALSE, "mucc: ");
  svn_error_clear(err);
  if (pool)
    svn_pool_destroy(pool);
  exit(EXIT_FAILURE);
}

static apr_pool_t *
init(const char *application)
{
  apr_allocator_t *allocator;
  apr_pool_t *pool;
  svn_error_t *err;
  const svn_version_checklist_t checklist[] = {
    {"svn_subr", svn_subr_version},
    {NULL, NULL}
  };

  SVN_VERSION_DEFINE(my_version);

  if (svn_cmdline_init(application, stderr) || apr_allocator_create(&allocator))
    exit(EXIT_FAILURE);

  err = svn_ver_check_list(&my_version, checklist);
  if (err)
    handle_error(err, NULL);

  apr_allocator_max_free_set(allocator, SVN_ALLOCATOR_RECOMMENDED_MAX_FREE);
  pool = svn_pool_create_ex(NULL, allocator);
  apr_allocator_owner_set(allocator, pool);

  return pool;
}

static void
usage(apr_pool_t *pool, int exit_val)
{
  FILE *stream = exit_val == EXIT_SUCCESS ? stdout : stderr;
  const char msg[]
    =
    "usage: deltagen [OPTIONS] SOURCE [TARGET]\n"
    "\n"
    "If TARGET is not specified, then deltagen will create a self-compressed\n"
    "svndiff stream.\n"
    "\n"
    "options:\n"
    "  -o, --output ARG    use ARG as the output stream\n"
    "  -v, --version [0|1] use diff format 0 or 1 (0 is the default)\n"
    "  -h, --help          display this text\n";
  svn_error_clear(svn_cmdline_fputs(msg, stream, pool));
  apr_pool_destroy(pool);
  exit(exit_val);
}

static svn_error_t *
create_stdio_stream(svn_stream_t **stream,
                    APR_DECLARE(apr_status_t) open_fn(apr_file_t **,
                                                      apr_pool_t *),
                    apr_pool_t *pool)
{
  apr_file_t *stdio_file;
  apr_status_t apr_err = open_fn(&stdio_file, pool);

  if (apr_err)
    return svn_error_wrap_apr(apr_err, "Can't open stdio file");

  *stream = svn_stream_from_aprfile(stdio_file, pool);
  return SVN_NO_ERROR;
}


static svn_error_t *
create_read_stream(svn_stream_t **out,
                   const char *filename,
                   apr_pool_t *pool)
{
  apr_status_t apr_err;
  apr_file_t *f;
  apr_status_t status;

  status = apr_file_open(&f, filename, APR_READ | APR_BINARY, APR_OS_DEFAULT, pool);
  if (status)
    return svn_error_wrap_apr(status, "open error");
  *out = svn_stream_from_aprfile2(f, FALSE, pool);

  return NULL;
}


static svn_error_t *
do_delta(svn_stream_t *output, svn_stream_t *source, svn_stream_t *target,
         int version, apr_pool_t *pool)
{
  svn_txdelta_window_handler_t handler;
  void *handler_baton;
  svn_txdelta_stream_t *delta_stream;

  svn_txdelta(&delta_stream, source, target, pool);

  svn_txdelta_to_svndiff2(&handler, &handler_baton, output, version, pool);

  SVN_ERR(svn_txdelta_send_txstream(delta_stream, handler, handler_baton, pool));

  return NULL;
}


int
main(int argc, const char **argv)
{
  apr_pool_t *pool = init("deltagen");
  const char *anchor = NULL;
  svn_error_t *err;
  apr_status_t apr_err;
  apr_file_t *f;
  apr_getopt_t *getopt;
  apr_size_t len;
  const apr_getopt_option_t options[] = {
    {"output", 'o', 1, ""},
    {"version", 'v', 1, ""},
    {"help", 'h', 0, ""},
    {NULL, 0, 0, NULL}
  };
  svn_stream_t *output_stream = NULL;
  svn_stream_t *source, *target;
  int version = 0;

  apr_getopt_init(&getopt, pool, argc, argv);
  getopt->interleave = 1;
  while (1)
    {
      int opt;
      const char *arg;
      apr_status_t status = apr_getopt_long(getopt, options, &opt, &arg);
      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        handle_error(svn_error_wrap_apr(status, "getopt failure"), pool);
      switch(opt)
        {
        case 'o':
          status = apr_file_open(&f, arg, APR_READ | APR_WRITE | APR_EXCL | APR_BINARY | APR_CREATE, APR_OS_DEFAULT, pool);
          if (status)
            handle_error(svn_error_wrap_apr(status, "open error"), pool);
          output_stream = svn_stream_from_aprfile2(f, FALSE, pool);
          break;
        case 'v':
          version = atoi(arg);
          break;
        case 'h':
          usage(pool, EXIT_SUCCESS);
        }
    }

  err = NULL;

  if (! output_stream)
  {
    err = create_stdio_stream(&output_stream, apr_file_open_stdout, pool);
    if (err)
      handle_error(err, pool);
  }

  if ((getopt->argc - getopt->ind) < 1 || (getopt->argc - getopt->ind) > 2)
  {
    usage(pool, EXIT_FAILURE);
  }

  err = create_read_stream(&source, getopt->argv[getopt->ind++], pool);
  if (err)
    handle_error(err, pool);

  if (getopt->argc != getopt->ind)
  {
    err = create_read_stream(&target, getopt->argv[getopt->ind++], pool);
    if (err)
      handle_error(err, pool);
  }
  else
  {
    target = source;
    source = svn_stream_empty(pool);
  }
  
  err = do_delta(output_stream, source, target, version, pool);

  svn_stream_close(output_stream);
  svn_stream_close(source);
  svn_stream_close(target);

  if (err)
    handle_error(err, pool);

  svn_pool_destroy(pool);
  return EXIT_SUCCESS;
}

