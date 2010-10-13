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
#include <assert.h>

static void handle_error(svn_error_t *err, apr_pool_t *pool)
{
  if (err)
    svn_handle_error2(err, stderr, FALSE, "undelta: ");
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
    "usage: undelta [OPTIONS] FILE\n"
    "\n"
    "options:\n"
    "  -o, --output ARG    use ARG as the output stream\n"
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
do_undelta(svn_stream_t *output, svn_stream_t *source, apr_pool_t *pool)
{
  svn_txdelta_window_handler_t handler;
  svn_txdelta_window_t *window;
  void *handler_baton;
  apr_size_t len = 4;
  char *buf = apr_pcalloc(pool, 4);
  int version;
  apr_size_t tlen;
  char *sbuf=NULL, *tbuf;
  apr_pool_t *iterpool1 = svn_pool_create(pool);
  apr_pool_t *iterpool2 = svn_pool_create(pool);
  int i;
  svn_error_t *error;
  
  SVN_ERR(svn_stream_read(source, buf, &len));

  /* ### Should do better than this */
  assert(len == 4);
  assert(strncmp("SVN", buf, 3) == 0);
  version = buf[3];
  assert(version == 0 || version == 1);

  for(i=0;;i++)
  {
    apr_pool_t *iterpool;
    if (i & 1)
      iterpool = iterpool2;
    else
      iterpool = iterpool1;

    svn_pool_clear(iterpool);
    
    error = svn_txdelta_read_svndiff_window(&window, source, version,
                                            iterpool);
    if (error && error->apr_err == SVN_ERR_SVNDIFF_UNEXPECTED_END)
    {
      break;
    }
    else
    {
      SVN_ERR(error);
    }

    if (NULL == window)
      break;

    if (! sbuf)
    {
      sbuf = apr_palloc(iterpool, window->sview_len);
    }
    
    /* Apply window to source. */
    tlen = window->tview_len;
    tbuf = apr_palloc(iterpool, tlen);

    svn_txdelta_apply_instructions(window, sbuf, tbuf, &tlen);
    if (tlen != window->tview_len)
      return svn_error_create(SVN_ERR_STREAM_MALFORMED_DATA, NULL,
                              "svndiff window length is corrupt");
    sbuf = tbuf;
    
    svn_stream_write(output, tbuf, &tlen);
  }
  
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

  err = do_undelta(output_stream, source, pool);

  svn_stream_close(output_stream);
  svn_stream_close(source);

  if (err)
    handle_error(err, pool);

  svn_pool_destroy(pool);
  return EXIT_SUCCESS;
}

