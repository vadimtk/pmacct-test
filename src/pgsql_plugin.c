/*
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2013 by Paolo Lucente
*/

/*
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
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#define __PGSQL_PLUGIN_C

/* includes */
#include "pmacct.h"
#include "pmacct-data.h"
#include "plugin_hooks.h"
#include "sql_common.h"
#include "pgsql_plugin.h"
#include "sql_common_m.c"

/* Functions */
void pgsql_plugin(int pipe_fd, struct configuration *cfgptr, void *ptr) 
{
  struct pkt_data *data;
  struct ports_table pt;
  struct pollfd pfd;
  struct insert_data idata;
  time_t refresh_deadline;
  int timeout;
  int ret, num;
  struct ring *rg = &((struct channels_list_entry *)ptr)->rg;
  struct ch_status *status = ((struct channels_list_entry *)ptr)->status;
  int datasize = ((struct channels_list_entry *)ptr)->datasize;
  u_int32_t bufsz = ((struct channels_list_entry *)ptr)->bufsize;
  struct networks_file_data nfd;
  char *dataptr;

  unsigned char *rgptr;
  int pollagain = TRUE;
  u_int32_t seq = 1, rg_err_count = 0;

  struct extra_primitives extras;
  struct primitives_ptrs prim_ptrs;

  memcpy(&config, cfgptr, sizeof(struct configuration));
  memcpy(&extras, &((struct channels_list_entry *)ptr)->extras, sizeof(struct extra_primitives));
  recollect_pipe_memory(ptr);
  pm_setproctitle("%s [%s]", "PostgreSQL Plugin", config.name);
  memset(&idata, 0, sizeof(idata));
  if (config.pidfile) write_pid_file_plugin(config.pidfile, config.type, config.name);
  if (config.logfile) {
    fclose(config.logfile_fd);
    config.logfile_fd = open_logfile(config.logfile);
  }

  sql_set_signals();
  sql_init_default_values(&extras);
  PG_init_default_values(&idata);
  PG_set_callbacks(&sqlfunc_cbr);
  sql_set_insert_func();

  /* some LOCAL initialization AFTER setting some default values */
  reload_map = FALSE;
  idata.now = time(NULL);
  refresh_deadline = idata.now;
  idata.cfg = &config;

  sql_init_maps(&extras, &prim_ptrs, &nt, &nc, &pt);
  sql_init_global_buffers();
  sql_init_pipe(&pfd, pipe_fd);
  sql_init_historical_acct(idata.now, &idata);
  sql_init_triggers(idata.now, &idata);
  sql_init_refresh_deadline(&refresh_deadline);

  /* building up static SQL clauses */
  idata.num_primitives = PG_compose_static_queries();
  glob_num_primitives = idata.num_primitives; 

  /* handling logfile template stuff */
  te = sql_init_logfile_template(&th);
  INIT_BUF(logbuf);

  /* handling purge preprocessor */
  set_preprocess_funcs(config.sql_preprocess, &prep);

  /* setting up environment variables */
  SQL_SetENV();

  sql_link_backend_descriptors(&bed, &p, &b);

  /* plugin main loop */
  for(;;) {
    poll_again:
    status->wakeup = TRUE;
    calc_refresh_timeout(refresh_deadline, idata.now, &timeout);
    ret = poll(&pfd, 1, timeout);

    if (ret <= 0) {
      if (getppid() == 1) {
        Log(LOG_ERR, "ERROR ( %s/%s ): Core process *seems* gone. Exiting.\n", config.name, config.type);
        exit_plugin(1);
      }

      if (ret < 0) goto poll_again;
    }

    idata.now = time(NULL);
    now = idata.now;

    if (config.sql_history) {
      while (idata.now > (idata.basetime + idata.timeslot)) {
        time_t saved_basetime = idata.basetime;

        idata.basetime += idata.timeslot;
        if (config.sql_history == COUNT_MONTHLY)
          idata.timeslot = calc_monthly_timeslot(idata.basetime, config.sql_history_howmany, ADD);
        glob_basetime = idata.basetime;
        idata.new_basetime = saved_basetime;
        glob_new_basetime = saved_basetime;
      }
    }

    switch (ret) {
    case 0: /* poll(): timeout */
      if (qq_ptr) sql_cache_flush(queries_queue, qq_ptr, &idata, FALSE);
      switch (ret = fork()) {
      case 0: /* Child */
	/* we have to ignore signals to avoid loops:
	   because we are already forked */
	signal(SIGINT, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	pm_setproctitle("%s [%s]", "PostgreSQL Plugin -- DB Writer", config.name);

	if (qq_ptr && sql_writers.flags != CHLD_ALERT) {
	  if (sql_writers.flags == CHLD_WARNING) sql_db_fail(&p);
	  (*sqlfunc_cbr.connect)(&p, NULL);
          (*sqlfunc_cbr.purge)(queries_queue, qq_ptr, &idata); 
	  (*sqlfunc_cbr.close)(&bed);
	}

	if (config.sql_trigger_exec) {
	  if (idata.now > idata.triggertime) sql_trigger_exec(config.sql_trigger_exec);
	}

        exit(0);
      default: /* Parent */
        if (ret == -1) { /* Something went wrong */
          Log(LOG_WARNING, "WARN ( %s/%s ): Unable to fork DB writer: %s\n", config.name, config.type, strerror(errno));
          sql_writers.active--;
        }

	if (pqq_ptr) sql_cache_flush_pending(pending_queries_queue, pqq_ptr, &idata);
	gettimeofday(&idata.flushtime, NULL);
	while (idata.now > refresh_deadline)
	  refresh_deadline += config.sql_refresh_time; 
	while (idata.now > idata.triggertime && idata.t_timeslot > 0) {
	  idata.triggertime  += idata.t_timeslot;
	  if (config.sql_trigger_time == COUNT_MONTHLY)
	    idata.t_timeslot = calc_monthly_timeslot(idata.triggertime, config.sql_trigger_time_howmany, ADD);
	}
	idata.new_basetime = FALSE;
	glob_new_basetime = FALSE;
	qq_ptr = pqq_ptr;
	memcpy(queries_queue, pending_queries_queue, qq_ptr*sizeof(struct db_cache *));

	if (reload_map) {
	  load_networks(config.networks_file, &nt, &nc);
	  load_ports(config.ports_file, &pt);
	  reload_map = FALSE;
	}
        break;
      }
      break;
    default: /* poll(): received data */
      read_data:
      if (!pollagain) {
        seq++;
        seq %= MAX_SEQNUM;
        if (seq == 0) rg_err_count = FALSE;
        idata.now = time(NULL);
	now = idata.now;
      }
      else {
        if ((ret = read(pipe_fd, &rgptr, sizeof(rgptr))) == 0)
          exit_plugin(1); /* we exit silently; something happened at the write end */
      }

      if (((struct ch_buf_hdr *)rg->ptr)->seq != seq) {
        if (!pollagain) {
          pollagain = TRUE;
          goto poll_again;
        }
        else {
          rg_err_count++;
          if (config.debug || (rg_err_count > MAX_RG_COUNT_ERR)) {
            Log(LOG_ERR, "ERROR ( %s/%s ): We are missing data.\n", config.name, config.type);
            Log(LOG_ERR, "If you see this message once in a while, discard it. Otherwise some solutions follow:\n");
            Log(LOG_ERR, "- increase shared memory size, 'plugin_pipe_size'; now: '%u'.\n", config.pipe_size);
            Log(LOG_ERR, "- increase buffer size, 'plugin_buffer_size'; now: '%u'.\n", config.buffer_size);
            Log(LOG_ERR, "- increase system maximum socket size.\n\n");
          }
          seq = ((struct ch_buf_hdr *)rg->ptr)->seq;
        }
      }

      pollagain = FALSE;
      memcpy(pipebuf, rg->ptr, bufsz);
      if ((rg->ptr+bufsz) >= rg->end) rg->ptr = rg->base;
      else rg->ptr += bufsz;

      /* lazy sql refresh handling */ 
      if (idata.now > refresh_deadline) {
        if (qq_ptr) sql_cache_flush(queries_queue, qq_ptr, &idata, FALSE);
        switch (ret = fork()) {
        case 0: /* Child */
          /* we have to ignore signals to avoid loops:
	     because we are already forked */
	  signal(SIGINT, SIG_IGN);
	  signal(SIGHUP, SIG_IGN);
	  pm_setproctitle("%s [%s]", "PostgreSQL Plugin -- DB Writer", config.name);

          if (qq_ptr && sql_writers.flags != CHLD_ALERT) {
	    if (sql_writers.flags == CHLD_WARNING) sql_db_fail(&p);
            (*sqlfunc_cbr.connect)(&p, NULL); 
            (*sqlfunc_cbr.purge)(queries_queue, qq_ptr, &idata);
	    (*sqlfunc_cbr.close)(&bed);
	  }

	  if (config.sql_trigger_exec) {
            if (idata.now > idata.triggertime) sql_trigger_exec(config.sql_trigger_exec);
          }

          exit(0);
        default: /* Parent */
          if (ret == -1) { /* Something went wrong */
            Log(LOG_WARNING, "WARN ( %s/%s ): Unable to fork DB writer: %s\n", config.name, config.type, strerror(errno));
            sql_writers.active--;
          }

	  if (pqq_ptr) sql_cache_flush_pending(pending_queries_queue, pqq_ptr, &idata);
	  gettimeofday(&idata.flushtime, NULL);
	  while (idata.now > refresh_deadline)
	    refresh_deadline += config.sql_refresh_time; 
	  while (idata.now > idata.triggertime && idata.t_timeslot > 0) {
            idata.triggertime  += idata.t_timeslot;
            if (config.sql_trigger_time == COUNT_MONTHLY)
              idata.t_timeslot = calc_monthly_timeslot(idata.triggertime, config.sql_trigger_time_howmany, ADD);
          }
	  idata.new_basetime = FALSE;
	  glob_new_basetime = FALSE;
	  qq_ptr = pqq_ptr;
	  memcpy(queries_queue, pending_queries_queue, qq_ptr*sizeof(struct db_cache *));

	  if (reload_map) {
	    load_networks(config.networks_file, &nt, &nc);
	    load_ports(config.ports_file, &pt);
	    reload_map = FALSE;
	  }
          break;
        }
      } 
      else {
        if (config.sql_trigger_exec) {
          while (idata.now > idata.triggertime && idata.t_timeslot > 0) {
            sql_trigger_exec(config.sql_trigger_exec);
	    idata.triggertime += idata.t_timeslot;
	    if (config.sql_trigger_time == COUNT_MONTHLY)
	      idata.t_timeslot = calc_monthly_timeslot(idata.triggertime, config.sql_trigger_time_howmany, ADD);
          }
        }
      }

      data = (struct pkt_data *) (pipebuf+sizeof(struct ch_buf_hdr));

      while (((struct ch_buf_hdr *)pipebuf)->num > 0) {
        for (num = 0; primptrs_funcs[num]; num++)
          (*primptrs_funcs[num])((u_char *)data, &extras, &prim_ptrs);

	for (num = 0; net_funcs[num]; num++)
	  (*net_funcs[num])(&nt, &nc, &data->primitives, prim_ptrs.pbgp, &nfd);

	if (config.ports_file) {
          if (!pt.table[data->primitives.src_port]) data->primitives.src_port = 0;
          if (!pt.table[data->primitives.dst_port]) data->primitives.dst_port = 0;
        }

        if (config.pkt_len_distrib_bins_str &&
            config.what_to_count_2 & COUNT_PKT_LEN_DISTRIB)
          evaluate_pkt_len_distrib(data);

        prim_ptrs.data = data;
        (*insert_func)(&prim_ptrs, &idata);

        ((struct ch_buf_hdr *)pipebuf)->num--;
        if (((struct ch_buf_hdr *)pipebuf)->num) {
          dataptr = (unsigned char *) data;
	  dataptr += datasize;
          data = (struct pkt_data *) dataptr;
	}
      }
      goto read_data;
    }
  }
}

int PG_cache_dbop_copy(struct DBdesc *db, struct db_cache *cache_elem, struct insert_data *idata)
{
  PGresult *ret;
  char *ptr_values, *ptr_where;
  char default_delim[] = ",", delim_buf[SRVBUFLEN];
  int num=0, have_flows=0;

  if (config.what_to_count & COUNT_FLOWS) have_flows = TRUE;

  if (!config.sql_delimiter)
    snprintf(delim_buf, SRVBUFLEN, "%s", default_delim);
  else
    snprintf(delim_buf, SRVBUFLEN, "%s", config.sql_delimiter);

  /* constructing SQL query */
  ptr_where = where_clause;
  ptr_values = values_clause;
  memset(where_clause, 0, sizeof(where_clause));
  memset(values_clause, 0, sizeof(values_clause));

  memcpy(&values, &copy_values, sizeof(values));
  while (num < idata->num_primitives) {
    (*where[num].handler)(cache_elem, idata, num, &ptr_values, &ptr_where);
    num++;
  }

#if defined HAVE_64BIT_COUNTERS
  if (have_flows) snprintf(ptr_values, SPACELEFT(values_clause), "%s%llu%s%llu%s%llu\n", delim_buf, cache_elem->packet_counter,
											delim_buf, cache_elem->bytes_counter,
											delim_buf, cache_elem->flows_counter);
  else snprintf(ptr_values, SPACELEFT(values_clause), "%s%llu%s%llu\n", delim_buf, cache_elem->packet_counter,
									delim_buf, cache_elem->bytes_counter);
#else
  if (have_flows) snprintf(ptr_values, SPACELEFT(values_clause), "%s%lu%s%lu%s%lu\n", delim_buf, cache_elem->packet_counter,
											delim_buf, cache_elem->bytes_counter,
											delim_buf, cache_elem->flows_counter);
  else snprintf(ptr_values, SPACELEFT(values_clause), "%s%lu%s%lu\n", delim_buf, cache_elem->packet_counter,
									delim_buf, cache_elem->bytes_counter);
#endif

  strncpy(sql_data, values_clause, SPACELEFT(sql_data));

  if (PQputCopyData(db->desc, sql_data, strlen(sql_data)) < 0) { // avoid strlen() 
    db->errmsg = PQerrorMessage(db->desc);
    Log(LOG_DEBUG, "DEBUG ( %s/%s ): FAILED query follows:\n%s\n", config.name, config.type, sql_data);
    if (db->errmsg) Log(LOG_ERR, "ERROR ( %s/%s ): %s\n", config.name, config.type, db->errmsg);
    sql_db_fail(db);

    return TRUE;
  }
  idata->iqn++;
  idata->een++;

  Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n", config.name, config.type, sql_data);

  return FALSE;
}

int PG_cache_dbop(struct DBdesc *db, struct db_cache *cache_elem, struct insert_data *idata)
{
  PGresult *ret;
  char *ptr_values, *ptr_where, *ptr_set, *ptr_insert;
  int num=0, num_set=0, have_flows=0;

  if (config.what_to_count & COUNT_FLOWS) have_flows = TRUE;

  /* constructing SQL query */
  ptr_where = where_clause;
  ptr_values = values_clause; 
  ptr_set = set_clause;
  ptr_insert = insert_full_clause;
  memset(where_clause, 0, sizeof(where_clause));
  memset(values_clause, 0, sizeof(values_clause));
  memset(set_clause, 0, sizeof(set_clause));
  memset(insert_full_clause, 0, sizeof(insert_full_clause));

  for (num = 0; num < idata->num_primitives; num++)
    (*where[num].handler)(cache_elem, idata, num, &ptr_values, &ptr_where);

  if (cache_elem->flow_type == NF9_FTYPE_EVENT) {
    for (num_set = 0; set_event[num_set].type; num_set++)
      (*set_event[num_set].handler)(cache_elem, idata, num_set, &ptr_set, NULL);
  }
  else {
    for (num_set = 0; set[num_set].type; num_set++)
      (*set[num_set].handler)(cache_elem, idata, num_set, &ptr_set, NULL);
  }

  /* sending UPDATE query a) if not switched off and
     b) if we actually have something to update */
  if (!config.sql_dont_try_update && num_set) {
    strncpy(sql_data, update_clause, SPACELEFT(sql_data));
    strncat(sql_data, set_clause, SPACELEFT(sql_data));
    strncat(sql_data, where_clause, SPACELEFT(sql_data));

    ret = PQexec(db->desc, sql_data);
    if (PQresultStatus(ret) != PGRES_COMMAND_OK) {
      db->errmsg = PQresultErrorMessage(ret);
      PQclear(ret);
      Log(LOG_DEBUG, "DEBUG ( %s/%s ): FAILED query follows:\n%s\n", config.name, config.type, sql_data);
      if (db->errmsg) Log(LOG_ERR, "ERROR ( %s/%s ): %s\n\n", config.name, config.type, db->errmsg);
      sql_db_fail(db);

      return TRUE;
    }
    PQclear(ret);
  }

  if (config.sql_dont_try_update || !num_set || (!PG_affected_rows(ret))) {
    /* UPDATE failed, trying with an INSERT query */ 
    if (cache_elem->flow_type == NF9_FTYPE_EVENT) {
      strncpy(insert_full_clause, insert_clause, SPACELEFT(insert_full_clause));
      strncat(insert_full_clause, insert_nocounters_clause, SPACELEFT(insert_full_clause));
      strncat(ptr_values, ")", SPACELEFT(values_clause));
    }
    else {
      strncpy(insert_full_clause, insert_clause, SPACELEFT(insert_full_clause));
      strncat(insert_full_clause, insert_counters_clause, SPACELEFT(insert_full_clause));
#if defined HAVE_64BIT_COUNTERS
      if (have_flows) snprintf(ptr_values, SPACELEFT(values_clause), ", %llu, %llu, %llu)", cache_elem->packet_counter, cache_elem->bytes_counter, cache_elem->flows_counter);
      else snprintf(ptr_values, SPACELEFT(values_clause), ", %llu, %llu)", cache_elem->packet_counter, cache_elem->bytes_counter);
#else
      if (have_flows) snprintf(ptr_values, SPACELEFT(values_clause), ", %lu, %lu, %lu)", cache_elem->packet_counter, cache_elem->bytes_counter, cache_elem->flows_counter);
      else snprintf(ptr_values, SPACELEFT(values_clause), ", %lu, %lu)", cache_elem->packet_counter, cache_elem->bytes_counter);
#endif
    }
    strncpy(sql_data, insert_full_clause, sizeof(sql_data));
    strncat(sql_data, values_clause, SPACELEFT(sql_data));

    ret = PQexec(db->desc, sql_data);
    if (PQresultStatus(ret) != PGRES_COMMAND_OK) {
      db->errmsg = PQresultErrorMessage(ret);
      PQclear(ret);
      Log(LOG_DEBUG, "DEBUG ( %s/%s ): FAILED query follows:\n%s\n", config.name, config.type, sql_data);
      if (db->errmsg) Log(LOG_ERR, "ERROR ( %s/%s ): %s\n\n", config.name, config.type, db->errmsg);
      sql_db_fail(db);

      return TRUE;
    }
    PQclear(ret);
    idata->iqn++;
  }
  else idata->uqn++;
  idata->een++;

  Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n\n", config.name, config.type, sql_data);

  return FALSE;
}

void PG_cache_purge(struct db_cache *queue[], int index, struct insert_data *idata)
{
  PGresult *ret;
  struct logfile lf;
  struct db_cache **reprocess_queries_queue, **bulk_reprocess_queries_queue;
  char orig_insert_clause[LONGSRVBUFLEN], orig_update_clause[LONGSRVBUFLEN], orig_lock_clause[LONGSRVBUFLEN];
  char orig_copy_clause[LONGSRVBUFLEN], tmpbuf[LONGLONGSRVBUFLEN], tmptable[SRVBUFLEN];
  time_t start;
  int j, r, reprocess = 0, stop, go_to_pending, reprocess_idx, bulk_reprocess_idx;
  struct primitives_ptrs prim_ptrs;
  struct pkt_data dummy_data;

  if (!index) return;

  bed.lf = &lf;
  memset(&lf, 0, sizeof(struct logfile));
  memset(&prim_ptrs, 0, sizeof(prim_ptrs));
  memset(&dummy_data, 0, sizeof(dummy_data));

  reprocess_queries_queue = (struct db_cache **) malloc(qq_size*sizeof(struct db_cache *));
  bulk_reprocess_queries_queue = (struct db_cache **) malloc(qq_size*sizeof(struct db_cache *));
  if (!reprocess_queries_queue || !bulk_reprocess_queries_queue) {
    Log(LOG_ERR, "ERROR ( %s/%s ): malloc() failed (reprocess_queries_queue). Exiting ..\n", config.name, config.type);
    exit_plugin(1);
  }

  for (j = 0, stop = 0; (!stop) && preprocess_funcs[j]; j++) 
    stop = preprocess_funcs[j](queue, &index, j);
  if (config.what_to_count & COUNT_CLASS)
    sql_invalidate_shadow_entries(queue, &index);
  idata->ten = index;

  Log(LOG_INFO, "INFO ( %s/%s ): *** Purging cache - START ***\n", config.name, config.type);
  start = time(NULL);

  /* re-using pending queries queue stuff from parent and saving clauses */
  memcpy(pending_queries_queue, queue, index*sizeof(struct db_cache *));
  pqq_ptr = index;

  strlcpy(orig_copy_clause, copy_clause, LONGSRVBUFLEN);
  strlcpy(orig_insert_clause, insert_clause, LONGSRVBUFLEN);
  strlcpy(orig_update_clause, update_clause, LONGSRVBUFLEN);
  strlcpy(orig_lock_clause, lock_clause, LONGSRVBUFLEN);

  start:
  memcpy(queue, pending_queries_queue, pqq_ptr*sizeof(struct db_cache *));
  memset(pending_queries_queue, 0, pqq_ptr*sizeof(struct db_cache *));
  index = pqq_ptr; pqq_ptr = 0;

  /* We check for variable substitution in SQL table */
  if (idata->dyn_table) {
    time_t stamp = 0;

    memset(tmpbuf, 0, LONGLONGSRVBUFLEN);
    stamp = queue[0]->basetime;

    prim_ptrs.data = &dummy_data;
    primptrs_set_all_from_db_cache(&prim_ptrs, queue[0]);

    strlcpy(idata->dyn_table_name, config.sql_table, SRVBUFLEN);
    strlcpy(insert_clause, orig_insert_clause, LONGSRVBUFLEN);
    strlcpy(update_clause, orig_update_clause, LONGSRVBUFLEN);
    strlcpy(lock_clause, orig_lock_clause, LONGSRVBUFLEN);

    handle_dynname_internal_strings_same(tmpbuf, LONGSRVBUFLEN, copy_clause, &prim_ptrs);
    handle_dynname_internal_strings_same(tmpbuf, LONGSRVBUFLEN, insert_clause, &prim_ptrs);
    handle_dynname_internal_strings_same(tmpbuf, LONGSRVBUFLEN, update_clause, &prim_ptrs);
    handle_dynname_internal_strings_same(tmpbuf, LONGSRVBUFLEN, lock_clause, &prim_ptrs);
    handle_dynname_internal_strings_same(tmpbuf, LONGSRVBUFLEN, idata->dyn_table_name, &prim_ptrs);

    strftime_same(copy_clause, LONGSRVBUFLEN, tmpbuf, &stamp);
    strftime_same(insert_clause, LONGSRVBUFLEN, tmpbuf, &stamp);
    strftime_same(update_clause, LONGSRVBUFLEN, tmpbuf, &stamp);
    strftime_same(lock_clause, LONGSRVBUFLEN, tmpbuf, &stamp);
    strftime_same(idata->dyn_table_name, LONGSRVBUFLEN, tmpbuf, &stamp);

    if (config.sql_table_schema) sql_create_table(bed.p, &stamp, &prim_ptrs); 
  }

  /* beginning DB transaction */
  (*sqlfunc_cbr.lock)(bed.p);

  /* for each element of the queue to be processed we execute sql_query(); the function
     returns a non-zero value if DB has failed; then we use reprocess_queries_queue and
     bulk_reprocess_queries_queue to handle reprocessing of specific elements or bulk
     queue (of elements not being held in a pending_queries_queue) due to final COMMIT
     failure */

  memset(reprocess_queries_queue, 0, qq_size*sizeof(struct db_cache *));
  memset(bulk_reprocess_queries_queue, 0, qq_size*sizeof(struct db_cache *));
  reprocess_idx = 0; bulk_reprocess_idx = 0;

  for (j = 0; j < index; j++) {
    go_to_pending = FALSE;

    if (idata->dyn_table) {
      time_t stamp = 0;

      memset(tmpbuf, 0, LONGLONGSRVBUFLEN); // XXX: pedantic?
      stamp = queue[idata->current_queue_elem]->basetime;
      strlcpy(tmptable, config.sql_table, SRVBUFLEN);

      prim_ptrs.data = &dummy_data;
      primptrs_set_all_from_db_cache(&prim_ptrs, queue[idata->current_queue_elem]);
      handle_dynname_internal_strings_same(tmpbuf, LONGSRVBUFLEN, tmptable, &prim_ptrs);
      strftime_same(tmptable, LONGSRVBUFLEN, tmpbuf, &stamp);

      if (strncmp(idata->dyn_table_name, tmptable, SRVBUFLEN)) {
        pending_queries_queue[pqq_ptr] = queue[idata->current_queue_elem];

        pqq_ptr++;
        go_to_pending = TRUE;
      }
    }

    if (!go_to_pending) { 
      if (queue[j]->valid) {
	r = sql_query(&bed, queue[j], idata);

	/* note down all elements in case of a reprocess due to COMMIT failure */
	bulk_reprocess_queries_queue[bulk_reprocess_idx] = queue[j];
	bulk_reprocess_idx++;
      }
      else r = FALSE; /* not valid elements are marked as not to be reprocessed */ 
      if (r) {
        reprocess_queries_queue[reprocess_idx] = queue[j];
        reprocess_idx++;

	if (!reprocess) sql_db_fail(&p);
        reprocess = REPROCESS_SPECIFIC;
      }
    }
  }

  /* Finalizing DB transaction */
  if (!p.fail) {
    if (config.sql_use_copy) {
      if (PQputCopyEnd(p.desc, NULL) < 0) Log(LOG_ERR, "ERROR ( %s/%s ): COPY failed!\n\n", config.name, config.type); 
    }

    ret = PQexec(p.desc, "COMMIT");
    if (PQresultStatus(ret) != PGRES_COMMAND_OK) {
      if (!reprocess) sql_db_fail(&p);
      reprocess = REPROCESS_BULK;
    }
    PQclear(ret);
  }

  /* don't reprocess free (SQL_CACHE_FREE) and already recovered (SQL_CACHE_ERROR) elements */
  if (p.fail) {
    if (reprocess = REPROCESS_SPECIFIC) {
      for (j = 0; j <= reprocess_idx; j++) {
        if (reprocess_queries_queue[j]->valid == SQL_CACHE_COMMITTED) sql_query(&bed, reprocess_queries_queue[j], idata);
      }
    }
    else if (reprocess = REPROCESS_BULK) {
      for (j = 0; j <= bulk_reprocess_idx; j++) {
        if (bulk_reprocess_queries_queue[j]->valid == SQL_CACHE_COMMITTED) sql_query(&bed, bulk_reprocess_queries_queue[j], idata);
      }
    }
  }

  if (b.connected) {
    if (config.sql_use_copy) {
      if (PQputCopyEnd(b.desc, NULL) < 0) Log(LOG_ERR, "ERROR ( %s/%s ): COPY failed!\n\n", config.name, config.type);
    }
    ret = PQexec(b.desc, "COMMIT");
    if (PQresultStatus(ret) != PGRES_COMMAND_OK) sql_db_fail(&b);
    PQclear(ret);
  }

  /* rewinding stuff */
  if (lf.file) PG_file_close(&lf);
  if (lf.fail || b.fail) Log(LOG_ALERT, "ALERT ( %s/%s ): recovery for PgSQL operation failed.\n", config.name, config.type);

  /* If we have pending queries then start again */
  if (pqq_ptr) goto start;

  idata->elap_time = time(NULL)-start;
  Log(LOG_INFO, "INFO ( %s/%s ): *** Purging cache - END (QN: %u, ET: %u) ***\n", config.name, config.type, idata->qn, idata->elap_time);

  if (config.sql_trigger_exec) {
    if (!config.debug) idata->elap_time = time(NULL)-start;
    SQL_SetENV_child(idata);
  }
}

int PG_evaluate_history(int primitive)
{
  if (config.sql_history) {
    if (primitive) {
      strncat(copy_clause, ", ", SPACELEFT(copy_clause));
      strncat(insert_clause, ", ", SPACELEFT(insert_clause));
      strncat(values[primitive].string, ", ", sizeof(values[primitive].string));
      strncat(where[primitive].string, " AND ", sizeof(where[primitive].string));
    }
    if (!config.sql_history_since_epoch)
      strncat(where[primitive].string, "ABSTIME(%u)::Timestamp::Timestamp without time zone = ", SPACELEFT(where[primitive].string));
    else
      strncat(where[primitive].string, "%u = ", SPACELEFT(where[primitive].string));
    strncat(where[primitive].string, "stamp_inserted", SPACELEFT(where[primitive].string));

    strncat(copy_clause, "stamp_updated, stamp_inserted", SPACELEFT(copy_clause));
    strncat(insert_clause, "stamp_updated, stamp_inserted", SPACELEFT(insert_clause));
    if (config.sql_use_copy) {
      char default_delim[] = ",", delim_buf[SRVBUFLEN];

      if (!config.sql_delimiter || !config.sql_use_copy)
        snprintf(delim_buf, SRVBUFLEN, "%s ", default_delim);
      else
        snprintf(delim_buf, SRVBUFLEN, "%s ", config.sql_delimiter);

      if (!config.sql_history_since_epoch) { 
	strncat(values[primitive].string, "%s", SPACELEFT(values[primitive].string));
	strncat(values[primitive].string, delim_buf, SPACELEFT(values[primitive].string));
	strncat(values[primitive].string, "%s", SPACELEFT(values[primitive].string));
        values[primitive].handler = where[primitive].handler = count_copy_timestamp_handler;
      }
      else {
	strncat(values[primitive].string, "%u", SPACELEFT(values[primitive].string));
	strncat(values[primitive].string, delim_buf, SPACELEFT(values[primitive].string));
	strncat(values[primitive].string, "%u", SPACELEFT(values[primitive].string));
        values[primitive].handler = where[primitive].handler = count_timestamp_handler;
      }
    }
    else {
      if (!config.sql_history_since_epoch)
	strncat(values[primitive].string, "ABSTIME(%u)::Timestamp, ABSTIME(%u)::Timestamp", SPACELEFT(values[primitive].string));
      else
	strncat(values[primitive].string, "%u, %u", SPACELEFT(values[primitive].string));
      values[primitive].handler = where[primitive].handler = count_timestamp_handler;
    }
    where[primitive].type = values[primitive].type = TIMESTAMP;

    primitive++;
  }

  return primitive;
}

int PG_compose_static_queries()
{
  int primitives=0, set_primitives=0, set_event_primitives=0, have_flows=0, lock=0;
  char default_delim[] = ",", delim_buf[SRVBUFLEN];

  if (config.what_to_count & COUNT_FLOWS || (config.sql_table_version >= 4 &&
                                             config.sql_table_version < SQL_TABLE_VERSION_BGP &&
                                             !config.sql_optimize_clauses)) {
    config.what_to_count |= COUNT_FLOWS;
    have_flows = TRUE;

    if ((config.sql_table_version < 4 || config.sql_table_version >= SQL_TABLE_VERSION_BGP) && !config.sql_optimize_clauses) {
      Log(LOG_ERR, "ERROR ( %s/%s ): The accounting of flows requires SQL table v4. Exiting.\n", config.name, config.type);
      exit_plugin(1);
    }
  }

  /* "INSERT INTO ... VALUES ... ", "COPY ... ", "... WHERE ..." stuff */
  strncpy(where[primitives].string, " WHERE ", sizeof(where[primitives].string));
  snprintf(copy_clause, sizeof(copy_clause), "COPY %s (", config.sql_table);
  snprintf(insert_clause, sizeof(insert_clause), "INSERT INTO %s (", config.sql_table);
  strncpy(values[primitives].string, " VALUES (", sizeof(values[primitives].string));
  primitives = PG_evaluate_history(primitives);
  primitives = sql_evaluate_primitives(primitives);

  strncat(copy_clause, ", packets, bytes", SPACELEFT(copy_clause));
  if (have_flows) strncat(copy_clause, ", flows", SPACELEFT(copy_clause));

  if (!config.sql_delimiter || !config.sql_use_copy)
    snprintf(delim_buf, SRVBUFLEN, ") FROM STDIN DELIMITER \'%s\'", default_delim);
  else
    snprintf(delim_buf, SRVBUFLEN, ") FROM STDIN DELIMITER \'%s\'", config.sql_delimiter);
  strncat(copy_clause, delim_buf, SPACELEFT(copy_clause));

  strncpy(insert_counters_clause, ", packets, bytes", SPACELEFT(insert_counters_clause));
  if (have_flows) strncat(insert_counters_clause, ", flows", SPACELEFT(insert_counters_clause));
  strncat(insert_counters_clause, ")", SPACELEFT(insert_counters_clause));
  strncpy(insert_nocounters_clause, ")", SPACELEFT(insert_nocounters_clause));

  /* "LOCK ..." stuff */
  
  if (config.sql_dont_try_update) snprintf(lock_clause, sizeof(lock_clause), "BEGIN;");
  else {
    if (config.sql_locking_style) lock = sql_select_locking_style(config.sql_locking_style); 
    switch (lock) {
    case PM_LOCK_ROW_EXCLUSIVE:
      snprintf(lock_clause, sizeof(lock_clause), "BEGIN; LOCK %s IN ROW EXCLUSIVE MODE;", config.sql_table);
      break;
    case PM_LOCK_EXCLUSIVE:
    default:
      snprintf(lock_clause, sizeof(lock_clause), "BEGIN; LOCK %s IN EXCLUSIVE MODE;", config.sql_table);
      break;
    }
  }

  /* "UPDATE ... SET ..." stuff */
  snprintf(update_clause, sizeof(update_clause), "UPDATE %s ", config.sql_table);

  set_primitives = sql_compose_static_set(have_flows);
  set_event_primitives = sql_compose_static_set_event();

  if (config.sql_history) {
    if (!config.sql_history_since_epoch) {
      strncpy(set[set_primitives].string, ", ", SPACELEFT(set[set_primitives].string));
      strncat(set[set_primitives].string, "stamp_updated=CURRENT_TIMESTAMP(0)", SPACELEFT(set[set_primitives].string)); 
      set[set_primitives].type = TIMESTAMP;
      set[set_primitives].handler = count_noop_setclause_handler;
      set_primitives++;

      if (set_event_primitives) strncpy(set_event[set_event_primitives].string, ", ", SPACELEFT(set_event[set_event_primitives].string));
      else strncpy(set_event[set_event_primitives].string, "SET ", SPACELEFT(set_event[set_event_primitives].string));
      strncat(set_event[set_event_primitives].string, "stamp_updated=CURRENT_TIMESTAMP(0)", SPACELEFT(set_event[set_event_primitives].string));
      set_event[set_event_primitives].type = TIMESTAMP;
      set_event[set_event_primitives].handler = count_noop_setclause_handler;
      set_event_primitives++;
    }
    else {
      strncpy(set[set_primitives].string, ", ", SPACELEFT(set[set_primitives].string));
      strncat(set[set_primitives].string, "stamp_updated=DATE_PART('epoch',NOW())::BIGINT", SPACELEFT(set[set_primitives].string));
      set[set_primitives].type = TIMESTAMP;
      set[set_primitives].handler = count_noop_setclause_handler;
      set_primitives++;

      if (set_event_primitives) strncpy(set_event[set_event_primitives].string, ", ", SPACELEFT(set_event[set_event_primitives].string));
      else strncpy(set_event[set_event_primitives].string, "SET ", SPACELEFT(set_event[set_event_primitives].string));
      strncat(set_event[set_event_primitives].string, "stamp_updated=DATE_PART('epoch',NOW())::BIGINT", SPACELEFT(set_event[set_event_primitives].string));
      set_event[set_event_primitives].type = TIMESTAMP;
      set_event[set_event_primitives].handler = count_noop_setclause_handler;
      set_primitives++;
    }
  }

  /* values for COPY */
  memcpy(&copy_values, &values, sizeof(copy_values));
  {
    int num, x, y;
    char *ptr;

    ptr = strchr(copy_values[0].string, '(');
    ptr++; strcpy(copy_values[0].string, ptr);

    for (num = 0; num < primitives; num++) {
      for (x = 0; copy_values[num].string[x] != '\0'; x++) {
	if (copy_values[num].string[x] == ' ' || copy_values[num].string[x] == '\'') {
	  for (y = x + 1; copy_values[num].string[y] != '\0'; y++)
            copy_values[num].string[y-1] = copy_values[num].string[y];
          copy_values[num].string[y-1] = '\0';
          x--;
        }
      }
      copy_values[num].string[x] = '\0';
    }
  }

  return primitives;
}

void PG_compose_conn_string(struct DBdesc *db, char *host)
{
  char *string;
  int slen = SRVBUFLEN;
  
  if (!db->conn_string) {
    db->conn_string = (char *) malloc(slen);
    if (!db->conn_string) {
      Log(LOG_ERR, "ERROR ( %s/%s ): malloc() failed (PG_compose_conn_string). Exiting ..\n", config.name, config.type);
      exit_plugin(1);
    }
    string = db->conn_string;

    snprintf(string, slen, "dbname=%s user=%s password=%s", config.sql_db, config.sql_user, config.sql_passwd);
    slen -= strlen(string);
    string += strlen(string);

    if (host) snprintf(string, slen, " host=%s", host);
  }
}

void PG_Lock(struct DBdesc *db)
{
  PGresult *PGret;

  if (!db->fail) {
    PGret = PQexec(db->desc, lock_clause);
    if (PQresultStatus(PGret) != PGRES_COMMAND_OK) {
      db->errmsg = PQresultErrorMessage(PGret);
      sql_db_errmsg(db);
      sql_db_fail(db);
    }
    PQclear(PGret);
    
    /* If using COPY, let's initialize it */
    if (config.sql_use_copy) {
      PGret = PQexec(db->desc, copy_clause);
      if (PQresultStatus(PGret) != PGRES_COPY_IN) {
	db->errmsg = PQresultErrorMessage(PGret);
	sql_db_errmsg(db);
	sql_db_fail(db);
      }
      else Log(LOG_DEBUG, "DEBUG ( %s/%s ): %s\n", config.name, config.type, copy_clause); 
      PQclear(PGret);
    }
  }
}

void PG_file_close(struct logfile *lf)
{
  if (logbuf.ptr != logbuf.base) {
    fwrite(logbuf.base, (logbuf.ptr-logbuf.base), 1, lf->file);
    logbuf.ptr = logbuf.base;
  }
  file_unlock(fileno(lf->file));
  fclose(lf->file);
}

void PG_DB_Connect(struct DBdesc *db, char *host)
{
  if (!db->fail) {
    db->desc = PQconnectdb(db->conn_string);
    if (PQstatus(db->desc) == CONNECTION_BAD) {
      char errmsg[64+SRVBUFLEN];

      sql_db_fail(db);
      strcpy(errmsg, "Failed connecting to ");
      strcat(errmsg, db->conn_string);
      db->errmsg = errmsg;
      sql_db_errmsg(db);
    }
    else sql_db_ok(db);
  }
}

void PG_DB_Close(struct BE_descs *bed)
{
  if (bed->p->connected) PQfinish(bed->p->desc);
  if (bed->b->connected) PQfinish(bed->b->desc);
}

void PG_create_dyn_table(struct DBdesc *db, char *buf)
{
  char *err_string;
  PGresult *PGret;

  if (!db->fail) {
    PGret = PQexec(db->desc, buf);
    if ((PQresultStatus(PGret) != PGRES_COMMAND_OK) && 
	(PQresultStatus(PGret) != PGRES_TUPLES_OK)) {
      err_string = PQresultErrorMessage(PGret);
      Log(LOG_DEBUG, "DEBUG ( %s/%s ): FAILED query follows:\n%s\n", config.name, config.type, buf);
      Log(LOG_ERR, "ERROR ( %s/%s ): %s\n\n", config.name, config.type, err_string);
    }
    PQclear(PGret);
  }
}

static int PG_affected_rows(PGresult *result)
{
  return atoi(PQcmdTuples(result));
}

void PG_create_backend(struct DBdesc *db)
{
  if (db->type == BE_TYPE_BACKUP) {
    if (!config.sql_backup_host) return;
  } 

  PG_compose_conn_string(db, config.sql_host);
}

void PG_set_callbacks(struct sqlfunc_cb_registry *cbr)
{
  memset(cbr, 0, sizeof(struct sqlfunc_cb_registry));

  cbr->connect = PG_DB_Connect;
  cbr->close = PG_DB_Close;
  cbr->lock = PG_Lock;
  /* cbr->unlock */ 
  if (!config.sql_use_copy) cbr->op = PG_cache_dbop;
  else cbr->op = PG_cache_dbop_copy;
  cbr->create_table = PG_create_dyn_table;
  cbr->purge = PG_cache_purge;
  cbr->create_backend = PG_create_backend;
}

void PG_init_default_values(struct insert_data *idata)
{
  /* Linking database parameters */
  if (!config.sql_data) config.sql_data = typed_str;
  if (!config.sql_user) config.sql_user = pgsql_user;
  if (!config.sql_db) config.sql_db = pgsql_db;
  if (!config.sql_passwd) config.sql_passwd = pgsql_pwd;
  if (!config.sql_table) {
    /* checking 'typed' table constraints */
    if (!strcmp(config.sql_data, "typed")) {
      if (config.what_to_count & (COUNT_SRC_AS|COUNT_SUM_AS|COUNT_DST_AS) && config.what_to_count &
	(COUNT_SRC_HOST|COUNT_SUM_HOST|COUNT_DST_HOST|COUNT_SRC_NET|COUNT_SUM_NET|COUNT_DST_NET) &&
	config.sql_table_version < 6) {
	Log(LOG_ERR, "ERROR ( %s/%s ): 'typed' PostgreSQL table in use: unable to mix HOST/NET and AS aggregations.\n", config.name, config.type);
	exit_plugin(1);
      }
      typed = TRUE;
    }
    else if (!strcmp(config.sql_data, "unified")) typed = FALSE;
    else {
      Log(LOG_ERR, "ERROR ( %s/%s ): Ignoring unknown 'sql_data' value '%s'.\n", config.name, config.type, config.sql_data);
      exit_plugin(1);
    }

    if (typed) {
      if (config.sql_table_version == (SQL_TABLE_VERSION_BGP+1)) config.sql_table = pgsql_table_bgp;
      else if (config.sql_table_version == 8) config.sql_table = pgsql_table_v8;
      else if (config.sql_table_version == 7) config.sql_table = pgsql_table_v7;
      else if (config.sql_table_version == 6) config.sql_table = pgsql_table_v6; 
      else if (config.sql_table_version == 5) {
        if (config.what_to_count & (COUNT_SRC_AS|COUNT_DST_AS|COUNT_SUM_AS)) config.sql_table = pgsql_table_as_v5;
        else config.sql_table = pgsql_table_v5;
      }
      else if (config.sql_table_version == 4) {
	if (config.what_to_count & (COUNT_SRC_AS|COUNT_DST_AS|COUNT_SUM_AS)) config.sql_table = pgsql_table_as_v4;
	else config.sql_table = pgsql_table_v4;
      }
      else if (config.sql_table_version == 3) {
	if (config.what_to_count & (COUNT_SRC_AS|COUNT_DST_AS|COUNT_SUM_AS)) config.sql_table = pgsql_table_as_v3;
	else config.sql_table = pgsql_table_v3;
      }
      else if (config.sql_table_version == 2) {
	if (config.what_to_count & (COUNT_SRC_AS|COUNT_DST_AS|COUNT_SUM_AS)) config.sql_table = pgsql_table_as_v2;
	else config.sql_table = pgsql_table_v2;
      }
      else {
	if (config.what_to_count & (COUNT_SRC_AS|COUNT_DST_AS|COUNT_SUM_AS)) config.sql_table = pgsql_table_as;
	else config.sql_table = pgsql_table;
      }
    }
    else {
      if (config.sql_table_version == 8) {
        Log(LOG_WARNING, "WARN ( %s/%s ): Unified data are no longer supported. Switching to typed data.\n", config.name, config.type);
        config.sql_table = pgsql_table_v8;
      }
      if (config.sql_table_version == 7) {
	Log(LOG_WARNING, "WARN ( %s/%s ): Unified data are no longer supported. Switching to typed data.\n", config.name, config.type);
	config.sql_table = pgsql_table_v7;
      }
      else if (config.sql_table_version == 6) {
	Log(LOG_WARNING, "WARN ( %s/%s ): Unified data are no longer supported. Switching to typed data.\n", config.name, config.type);
	config.sql_table = pgsql_table_v6;
      }
      else if (config.sql_table_version == 5) config.sql_table = pgsql_table_uni_v5;
      else if (config.sql_table_version == 4) config.sql_table = pgsql_table_uni_v4;
      else if (config.sql_table_version == 3) config.sql_table = pgsql_table_uni_v3;
      else if (config.sql_table_version == 2) config.sql_table = pgsql_table_uni_v2;
      else config.sql_table = pgsql_table_uni;
    }
  }
  if (strchr(config.sql_table, '%') || strchr(config.sql_table, '$')) idata->dyn_table = TRUE;
  glob_dyn_table = idata->dyn_table;

  if (config.sql_backup_host || config.sql_recovery_logfile) idata->recover = TRUE;
  if (!config.sql_dont_try_update && config.sql_use_copy) config.sql_use_copy = FALSE; 

  if (config.sql_locking_style) idata->locks = sql_select_locking_style(config.sql_locking_style);
}
