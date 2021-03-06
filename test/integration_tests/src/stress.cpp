/*
  Copyright (c) 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#define BOOST_TEST_DYN_LINK
#ifdef STAND_ALONE
#   define BOOST_TEST_MODULE cassandra
#endif

#include <algorithm>

#if !defined(WIN32) && !defined(_WIN32)
#include <signal.h>
#endif

#include <boost/test/unit_test.hpp>
#include <boost/test/debug.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/cstdint.hpp>
#include <boost/format.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/thread/future.hpp>
#include <boost/bind.hpp>
#include <boost/chrono.hpp>

#include "cassandra.h"
#include "test_utils.hpp"
#include "cql_ccm_bridge.hpp"

struct StressTests : public test_utils::MultipleNodesTest {
  StressTests() : MultipleNodesTest(3, 0) {
// TODO(mpenick): This is a stopgap. To be fixed in CPP-140
#if !defined(WIN32) && !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
  }
};

BOOST_FIXTURE_TEST_SUITE(stress, StressTests)

bool bind_and_execute_insert(CassSession* session, CassStatement* statement) {
  boost::chrono::system_clock::time_point now(boost::chrono::system_clock::now());
  boost::chrono::milliseconds event_time(boost::chrono::duration_cast<boost::chrono::milliseconds>(now.time_since_epoch()));
  std::string text_sample(test_utils::string_from_time_point(now));

  cass_statement_bind_uuid(statement, 0, test_utils::generate_time_uuid().uuid);
  cass_statement_bind_int64(statement, 1, event_time.count());
  cass_statement_bind_string(statement, 2, cass_string_init2(text_sample.data(), text_sample.size()));

  test_utils::CassFuturePtr future(cass_session_execute(session, statement));
  cass_future_wait(future.get());
  CassError code = cass_future_error_code(future.get());
  if(code != CASS_OK && code != CASS_ERROR_LIB_REQUEST_TIMED_OUT) { // Timeout is okay
    CassString message = cass_future_error_message(future.get());
    fprintf(stderr, "Error occured during insert '%.*s'\n", static_cast<int>(message.length), message.data);
    return false;
  }

  return true;
}

bool insert_task(CassSession* session, const std::string& query, CassConsistency consistency, int rows_per_id) {
  bool is_successful = true;
  for(int i = 0; i < rows_per_id; ++i) {
    test_utils::CassStatementPtr statement(cass_statement_new(cass_string_init(query.c_str()), 3));
    cass_statement_set_consistency(statement.get(), consistency);
    if(!bind_and_execute_insert(session, statement.get())) {
      is_successful = false;
    }
  }
  return is_successful;
}

bool insert_prepared_task(CassSession* session, const CassPrepared* prepared, CassConsistency consistency, int rows_per_id) {
  bool is_successful = true;
  for(int i = 0; i < rows_per_id; ++i) {
    test_utils::CassStatementPtr statement(cass_prepared_bind(prepared));
    cass_statement_set_consistency(statement.get(), consistency);
    if(!bind_and_execute_insert(session, statement.get())) {
      is_successful = false;
    }
  }
  return is_successful;
}

bool select_task(CassSession* session, const std::string& query, CassConsistency consistency, int num_iterations) {
  bool is_successful = true;

  test_utils::CassStatementPtr statement(cass_statement_new(cass_string_init(query.c_str()), 0));
  cass_statement_set_consistency(statement.get(), consistency);
  for(int i = 0; i < num_iterations; ++i) {
    test_utils::CassFuturePtr future(cass_session_execute(session, statement.get()));
    cass_future_wait(future.get());

    CassError code = cass_future_error_code(future.get());
    if(code != CASS_OK
       && code != CASS_ERROR_LIB_REQUEST_TIMED_OUT
       && code != CASS_ERROR_SERVER_READ_TIMEOUT) { // Timeout is okay
      CassString message = cass_future_error_message(future.get());
      fprintf(stderr, "Error occured during select '%.*s'\n", static_cast<int>(message.length), message.data);
      is_successful = false;
    }

    if(code == CASS_OK) {
      test_utils::CassResultPtr result(cass_future_get_result(future.get()));
      if(cass_result_row_count(result.get()) == 0) {
        fprintf(stderr, "No rows returned from query\n");
        is_successful = false;
      }
    }
  }

  return is_successful;
}

bool kill_task(boost::shared_ptr<cql::cql_ccm_bridge_t> ccm) {
  boost::this_thread::sleep_for(boost::chrono::milliseconds(300));
  ccm->kill(2);
  return true;
}

bool is_failed(boost::shared_future<bool> future) {
  return future.get() == false;
}

BOOST_AUTO_TEST_CASE(parallel_insert_and_select)
{
  test_utils::CassFuturePtr session_future(cass_cluster_connect(cluster));
  test_utils::wait_and_check_error(session_future.get());
  test_utils::CassSessionPtr session(cass_future_get_session(session_future.get()));

  test_utils::execute_query(session.get(), "CREATE KEYSPACE tester WITH replication = {'class': 'SimpleStrategy', 'replication_factor' : 3};");
  test_utils::execute_query(session.get(), "USE tester;");

  std::string table_name = str(boost::format("table_%s") % test_utils::generate_unique_str());

  test_utils::execute_query(session.get(), str(boost::format(test_utils::CREATE_TABLE_TIME_SERIES) % table_name));

  std::string insert_query = str(boost::format("INSERT INTO %s (id, event_time, text_sample) VALUES (?, ?, ?)") % table_name);
  std::string select_query = str(boost::format("SELECT * FROM %s LIMIT 10000") % table_name);

  test_utils::CassFuturePtr prepared_future(cass_session_prepare(session.get(),
                                                                 cass_string_init2(insert_query.data(), insert_query.size())));

  test_utils::wait_and_check_error(prepared_future.get());
  test_utils::CassPreparedPtr prepared(cass_future_get_prepared(prepared_future.get()));

  int rows_per_id = 100;
  int num_iterations = 10;

  insert_task(session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id);
  select_task(session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations);

  std::vector<boost::shared_future<bool> > futures;

  for(int i = 0; i < 10; ++i) {
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_task, session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(select_task, session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());

    futures.push_back(boost::async(boost::launch::async, boost::bind(select_task, session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_task, session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_task, session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id)).share());

    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(select_task, session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations)).share());
  }

  boost::wait_for_all(futures.begin(), futures.end());
  BOOST_REQUIRE(std::find_if(futures.begin(), futures.end(), is_failed) == futures.end());
}

BOOST_AUTO_TEST_CASE(parallel_insert_and_select_with_nodes_failing)
{
  test_utils::CassFuturePtr session_future(cass_cluster_connect(cluster));
  test_utils::wait_and_check_error(session_future.get());
  test_utils::CassSessionPtr session(cass_future_get_session(session_future.get()));

  test_utils::execute_query(session.get(), "CREATE KEYSPACE tester WITH replication = {'class': 'SimpleStrategy', 'replication_factor' : 3};");
  test_utils::execute_query(session.get(), "USE tester;");

  std::string table_name = str(boost::format("table_%s") % test_utils::generate_unique_str());

  test_utils::execute_query(session.get(), str(boost::format(test_utils::CREATE_TABLE_TIME_SERIES) % table_name));

  std::string insert_query = str(boost::format("INSERT INTO %s (id, event_time, text_sample) VALUES (?, ?, ?)") % table_name);
  std::string select_query = str(boost::format("SELECT * FROM %s LIMIT 10000") % table_name);

  test_utils::CassFuturePtr prepared_future(cass_session_prepare(session.get(),
                                                                 cass_string_init2(insert_query.data(), insert_query.size())));

  test_utils::wait_and_check_error(prepared_future.get());
  test_utils::CassPreparedPtr prepared(cass_future_get_prepared(prepared_future.get()));

  int rows_per_id = 100;
  int num_iterations = 10;

  insert_task(session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id);
  select_task(session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations);

  std::vector<boost::shared_future<bool> > futures;

  for(int i = 0; i < 10; ++i) {
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_task, session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(select_task, session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());

    futures.push_back(boost::async(boost::launch::async, boost::bind(select_task, session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_task, session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_task, session.get(), insert_query, CASS_CONSISTENCY_QUORUM, rows_per_id)).share());

    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(insert_prepared_task, session.get(), prepared.get(), CASS_CONSISTENCY_QUORUM, rows_per_id)).share());
    futures.push_back(boost::async(boost::launch::async, boost::bind(select_task, session.get(), select_query, CASS_CONSISTENCY_QUORUM, num_iterations)).share());
  }

  futures.insert(futures.begin() + 8, boost::async(boost::launch::async, boost::bind(kill_task, ccm)).share());

  boost::wait_for_all(futures.begin(), futures.end());
  BOOST_REQUIRE(std::find_if(futures.begin(), futures.end(), is_failed) == futures.end());
}

BOOST_AUTO_TEST_SUITE_END()
