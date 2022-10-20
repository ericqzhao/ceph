// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 * Copyright (C) 2017 OVH
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/labeled_perf_counters.h"
#include "common/dout.h"
#include "common/valgrind.h"
#include "include/common_fwd.h"

using std::ostringstream;
using std::make_pair;
using std::pair;

namespace TOPNSPC::common {
LabeledPerfCountersCollectionImpl::LabeledPerfCountersCollectionImpl()
{
}

LabeledPerfCountersCollectionImpl::~LabeledPerfCountersCollectionImpl()
{
  clear();
}

void LabeledPerfCountersCollectionImpl::add(LabeledPerfCounters *l)
{
  // make sure the name is unique
  labeled_perf_counters_set_t::iterator i;
  i = m_loggers.find(l);
  while (i != m_loggers.end()) {
    ostringstream ss;
    ss << l->get_name() << "-" << (void*)l;
    l->set_name(ss.str());
    i = m_loggers.find(l);
  }

  m_loggers.insert(l);

  for (unsigned int i = 0; i < l->m_data.size(); ++i) {
    LabeledPerfCounters::perf_counter_data_any_d &data = l->m_data[i];

    std::string path = l->get_name();
    path += ".";
    path += data.name;

    by_path[path] = {&data, l};
  }
}

void LabeledPerfCountersCollectionImpl::remove(LabeledPerfCounters *l)
{
  for (unsigned int i = 0; i < l->m_data.size(); ++i) {
    LabeledPerfCounters::perf_counter_data_any_d &data = l->m_data[i];

    std::string path = l->get_name();
    path += ".";
    path += data.name;

    by_path.erase(path);
  }

  labeled_perf_counters_set_t::iterator i = m_loggers.find(l);
  ceph_assert(i != m_loggers.end());
  m_loggers.erase(i);
}

void LabeledPerfCountersCollectionImpl::clear()
{
  labeled_perf_counters_set_t::iterator i = m_loggers.begin();
  labeled_perf_counters_set_t::iterator i_end = m_loggers.end();
  for (; i != i_end; ) {
    delete *i;
    m_loggers.erase(i++);
  }

  by_path.clear();
}

bool LabeledPerfCountersCollectionImpl::reset(const std::string &name)
{
  bool result = false;
  labeled_perf_counters_set_t::iterator i = m_loggers.begin();
  labeled_perf_counters_set_t::iterator i_end = m_loggers.end();

  if (!strcmp(name.c_str(), "all"))  {
    while (i != i_end) {
      (*i)->reset();
      ++i;
    }
    result = true;
  } else {
    while (i != i_end) {
      if (!name.compare((*i)->get_name())) {
	(*i)->reset();
	result = true;
	break;
      }
      ++i;
    }
  }

  return result;
}


/**
 * Serialize current values of performance counters.  Optionally
 * output the schema instead, or filter output to a particular
 * LabeledPerfCounters or particular named counter.
 *
 * @param logger name of subsystem logger, e.g. "mds_cache", may be empty
 * @param counter name of counter within subsystem, e.g. "num_strays",
 *                may be empty.
 * @param schema if true, output schema instead of current data.
 * @param histograms if true, dump histogram values,
 *                   if false dump all non-histogram counters
 */
void LabeledPerfCountersCollectionImpl::dump_formatted_generic(
    Formatter *f,
    bool schema,
    bool histograms,
    const std::string &logger,
    const std::string &counter) const
{
  f->open_object_section("perfcounter_collection");
  
  for (labeled_perf_counters_set_t::iterator l = m_loggers.begin();
       l != m_loggers.end(); ++l) {
    // Optionally filter on logger name, pass through counter filter
    if (logger.empty() || (*l)->get_name() == logger) {
      (*l)->dump_formatted_generic(f, schema, histograms, counter);
    }
  }
  f->close_section();
}

void LabeledPerfCountersCollectionImpl::with_counters(std::function<void(
      const LabeledPerfCountersCollectionImpl::CounterMap &)> fn) const
{
  fn(by_path);
}

// ---------------------------

LabeledPerfCounters::~LabeledPerfCounters()
{
}

void LabeledPerfCounters::inc(int idx, uint64_t amt)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_U64))
    return;
  if (data.type & PERFCOUNTER_LONGRUNAVG) {
    data.avgcount++;
    data.u64 += amt;
    data.avgcount2++;
  } else {
    data.u64 += amt;
  }
  data.accessed = true;
}

void LabeledPerfCounters::dec(int idx, uint64_t amt)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  ceph_assert(!(data.type & PERFCOUNTER_LONGRUNAVG));
  if (!(data.type & PERFCOUNTER_U64))
    return;
  data.u64 -= amt;
}

void LabeledPerfCounters::set(int idx, uint64_t amt)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_U64))
    return;

  ANNOTATE_BENIGN_RACE_SIZED(&data.u64, sizeof(data.u64),
                             "perf counter atomic");
  if (data.type & PERFCOUNTER_LONGRUNAVG) {
    data.avgcount++;
    data.u64 = amt;
    data.avgcount2++;
  } else {
    data.u64 = amt;
  }
}

uint64_t LabeledPerfCounters::get(int idx) const
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return 0;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  const perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_U64))
    return 0;
  return data.u64;
}

void LabeledPerfCounters::tinc(int idx, utime_t amt)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_TIME))
    return;
  if (data.type & PERFCOUNTER_LONGRUNAVG) {
    data.avgcount++;
    data.u64 += amt.to_nsec();
    data.avgcount2++;
  } else {
    data.u64 += amt.to_nsec();
  }
}

void LabeledPerfCounters::tinc(int idx, ceph::timespan amt)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_TIME))
    return;
  if (data.type & PERFCOUNTER_LONGRUNAVG) {
    data.avgcount++;
    data.u64 += amt.count();
    data.avgcount2++;
  } else {
    data.u64 += amt.count();
  }
}

void LabeledPerfCounters::tset(int idx, utime_t amt)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_TIME))
    return;
  data.u64 = amt.to_nsec();
  if (data.type & PERFCOUNTER_LONGRUNAVG)
    ceph_abort();
}

utime_t LabeledPerfCounters::tget(int idx) const
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return utime_t();
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  const perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_TIME))
    return utime_t();
  uint64_t v = data.u64;
  return utime_t(v / 1000000000ull, v % 1000000000ull);
}

void LabeledPerfCounters::hinc(int idx, int64_t x, int64_t y)
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return;
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);

  perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  ceph_assert(data.type == (PERFCOUNTER_HISTOGRAM | PERFCOUNTER_COUNTER | PERFCOUNTER_U64));
  ceph_assert(data.histogram);

  data.histogram->inc(x, y);
}

pair<uint64_t, uint64_t> LabeledPerfCounters::get_tavg_ns(int idx) const
{
#ifndef WITH_SEASTAR
  if (!m_cct->_conf->perf)
    return make_pair(0, 0);
#endif

  ceph_assert(idx > m_lower_bound);
  ceph_assert(idx < m_upper_bound);
  const perf_counter_data_any_d& data(m_data[idx - m_lower_bound - 1]);
  if (!(data.type & PERFCOUNTER_TIME))
    return make_pair(0, 0);
  if (!(data.type & PERFCOUNTER_LONGRUNAVG))
    return make_pair(0, 0);
  pair<uint64_t,uint64_t> a = data.read_avg();
  return make_pair(a.second, a.first);
}

void LabeledPerfCounters::reset()
{
  perf_counter_data_vec_t::iterator d = m_data.begin();
  perf_counter_data_vec_t::iterator d_end = m_data.end();

  while (d != d_end) {
    d->reset();
    ++d;
  }
}

void LabeledPerfCounters::dump_formatted_generic(Formatter *f, bool schema,
    bool histograms, const std::string &counter) const
{
  f->open_object_section(m_name.c_str());
  
  for (perf_counter_data_vec_t::const_iterator d = m_data.begin();
       d != m_data.end(); ++d) {
    if (!counter.empty() && counter != d->name) {
      // Optionally filter on counter name
      continue;
    }

    // Switch between normal and histogram view
    bool is_histogram = (d->type & PERFCOUNTER_HISTOGRAM) != 0;
    if (is_histogram != histograms) {
      continue;
    }

    if (schema) {
      f->open_object_section(d->name);
      // we probably should not have exposed this raw field (with bit
      // values), but existing plugins rely on it so we're stuck with
      // it.
      f->dump_int("type", d->type);

      if (d->type & PERFCOUNTER_COUNTER) {
	f->dump_string("metric_type", "counter");
      } else {
	f->dump_string("metric_type", "gauge");
      }

      if (d->type & PERFCOUNTER_LONGRUNAVG) {
	if (d->type & PERFCOUNTER_TIME) {
	  f->dump_string("value_type", "real-integer-pair");
	} else {
	  f->dump_string("value_type", "integer-integer-pair");
	}
      } else if (d->type & PERFCOUNTER_HISTOGRAM) {
	if (d->type & PERFCOUNTER_TIME) {
	  f->dump_string("value_type", "real-2d-histogram");
	} else {
	  f->dump_string("value_type", "integer-2d-histogram");
	}
      } else {
	if (d->type & PERFCOUNTER_TIME) {
	  f->dump_string("value_type", "real");
	} else {
	  f->dump_string("value_type", "integer");
	}
      }

      f->dump_string("description", d->description ? d->description : "");
      if (d->nick != NULL) {
        f->dump_string("nick", d->nick);
      } else {
        f->dump_string("nick", "");
      }
      f->dump_int("priority", get_adjusted_priority(d->prio));
      
      if (d->unit == UNIT_NONE) {
	f->dump_string("units", "none"); 
      } else if (d->unit == UNIT_BYTES) {
	f->dump_string("units", "bytes");
      }
      f->close_section();
    } else {
      if (d->type & PERFCOUNTER_LONGRUNAVG) {
	f->open_object_section(d->name);
	pair<uint64_t,uint64_t> a = d->read_avg();
	if (d->type & PERFCOUNTER_U64) {
	  f->dump_unsigned("avgcount", a.second);
	  f->dump_unsigned("sum", a.first);
	} else if (d->type & PERFCOUNTER_TIME) {
	  f->dump_unsigned("avgcount", a.second);
	  f->dump_format_unquoted("sum", "%" PRId64 ".%09" PRId64,
				  a.first / 1000000000ull,
				  a.first % 1000000000ull);
          uint64_t count = a.second;
          uint64_t sum_ns = a.first;
          if (count) {
            uint64_t avg_ns = sum_ns / count;
            f->dump_format_unquoted("avgtime", "%" PRId64 ".%09" PRId64,
                                    avg_ns / 1000000000ull,
                                    avg_ns % 1000000000ull);
          } else {
            f->dump_format_unquoted("avgtime", "%" PRId64 ".%09" PRId64, 0, 0);
          }
	} else {
	  ceph_abort();
	}
	f->close_section();
      } else if (d->type & PERFCOUNTER_HISTOGRAM) {
        ceph_assert(d->type == (PERFCOUNTER_HISTOGRAM | PERFCOUNTER_COUNTER | PERFCOUNTER_U64));
        ceph_assert(d->histogram);
        f->open_object_section(d->name);
        d->histogram->dump_formatted(f);
        f->close_section();
      } else {
        if(d->accessed) {
          uint64_t v = d->u64;
          if (d->type & PERFCOUNTER_U64) {
            f->dump_unsigned(d->name, v);
          } else if (d->type & PERFCOUNTER_TIME) {
            f->dump_format_unquoted(d->name, "%" PRId64 ".%09" PRId64,
                                    v / 1000000000ull,
                                    v % 1000000000ull);
          } else {
            ceph_abort();
          }
        }
      }
    }
  }
  f->close_section();
}

const std::string &LabeledPerfCounters::get_name() const
{
  return m_name;
}

LabeledPerfCounters::LabeledPerfCounters(CephContext *cct, const std::string &name,
	   int lower_bound, int upper_bound)
  : m_cct(cct),
    m_lower_bound(lower_bound),
    m_upper_bound(upper_bound),
    m_name(name)
#if !defined(WITH_SEASTAR) || defined(WITH_ALIEN)
    ,
    m_lock_name(std::string("LabeledPerfCounters::") + name.c_str()),
    m_lock(ceph::make_mutex(m_lock_name))
#endif
{
  m_data.resize(upper_bound - lower_bound - 1);
}

LabeledPerfCountersBuilder::LabeledPerfCountersBuilder(CephContext *cct, const std::string &name,
                  int first, int last)
  : m_perf_counters(new LabeledPerfCounters(cct, name, first, last))
{
}

LabeledPerfCountersBuilder::~LabeledPerfCountersBuilder()
{
  if (m_perf_counters)
    delete m_perf_counters;
  m_perf_counters = NULL;
}

void LabeledPerfCountersBuilder::add_u64_counter(
  int idx, const char *name,
  const char *description, const char *nick, int prio, int unit)
{
  add_impl(idx, name, description, nick, prio,
	   PERFCOUNTER_U64 | PERFCOUNTER_COUNTER, unit);
}

void LabeledPerfCountersBuilder::add_u64(
  int idx, const char *name,
  const char *description, const char *nick, int prio, int unit)
{
  add_impl(idx, name, description, nick, prio, PERFCOUNTER_U64, unit);
}

void LabeledPerfCountersBuilder::add_u64_avg(
  int idx, const char *name,
  const char *description, const char *nick, int prio, int unit)
{
  add_impl(idx, name, description, nick, prio,
	   PERFCOUNTER_U64 | PERFCOUNTER_LONGRUNAVG, unit);
}

void LabeledPerfCountersBuilder::add_time(
  int idx, const char *name,
  const char *description, const char *nick, int prio)
{
  add_impl(idx, name, description, nick, prio, PERFCOUNTER_TIME);
}

void LabeledPerfCountersBuilder::add_time_avg(
  int idx, const char *name,
  const char *description, const char *nick, int prio)
{
  add_impl(idx, name, description, nick, prio,
	   PERFCOUNTER_TIME | PERFCOUNTER_LONGRUNAVG);
}

void LabeledPerfCountersBuilder::add_u64_counter_histogram(
  int idx, const char *name,
  PerfHistogramCommon::axis_config_d x_axis_config,
  PerfHistogramCommon::axis_config_d y_axis_config,
  const char *description, const char *nick, int prio, int unit)
{
  add_impl(idx, name, description, nick, prio,
	   PERFCOUNTER_U64 | PERFCOUNTER_HISTOGRAM | PERFCOUNTER_COUNTER, unit,
           std::unique_ptr<PerfHistogram<>>{new PerfHistogram<>{x_axis_config, y_axis_config}});
}

void LabeledPerfCountersBuilder::add_impl(
  int idx, const char *name,
  const char *description, const char *nick, int prio, int ty, int unit,
  std::unique_ptr<PerfHistogram<>> histogram)
{
  ceph_assert(idx > m_perf_counters->m_lower_bound);
  ceph_assert(idx < m_perf_counters->m_upper_bound);
  LabeledPerfCounters::perf_counter_data_vec_t &vec(m_perf_counters->m_data);
  LabeledPerfCounters::perf_counter_data_any_d
    &data(vec[idx - m_perf_counters->m_lower_bound - 1]);
  ceph_assert(data.type == PERFCOUNTER_NONE);
  data.name = name;
  data.description = description;
  // nick must be <= 4 chars
  if (nick) {
    ceph_assert(strlen(nick) <= 4);
  }
  data.nick = nick;
  data.prio = prio ? prio : prio_default;
  data.type = (enum perfcounter_type_d)ty;
  data.unit = (enum unit_t) unit;
  data.histogram = std::move(histogram);
}

LabeledPerfCounters *LabeledPerfCountersBuilder::create_perf_counters()
{
  LabeledPerfCounters::perf_counter_data_vec_t::const_iterator d = m_perf_counters->m_data.begin();
  LabeledPerfCounters::perf_counter_data_vec_t::const_iterator d_end = m_perf_counters->m_data.end();
  for (; d != d_end; ++d) {
    ceph_assert(d->type != PERFCOUNTER_NONE);
    ceph_assert(d->type & (PERFCOUNTER_U64 | PERFCOUNTER_TIME));
  }

  LabeledPerfCounters *ret = m_perf_counters;
  m_perf_counters = NULL;
  return ret;
}

}