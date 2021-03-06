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

#include <boost/test/unit_test.hpp>
#include <boost/test/debug.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/cstdint.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>

#include "cassandra.h"
#include "test_utils.hpp"

namespace {

struct CallbackData {
  CallbackData()
    : was_called(false)
    , row_count(0) {}

  void wait() {
    boost::unique_lock<boost::mutex> lock(mutex);
    while (!was_called) {
      cond.wait(lock);
    }
  }

  void notify() {
    boost::unique_lock<boost::mutex> lock(mutex);
    was_called = true;
    cond.notify_one();
  }

  boost::mutex mutex;
  boost::condition cond;
  bool was_called;
  cass_size_t row_count;
};

void check_callback(CassFuture* future, void* data) {
  CallbackData* callback_data = reinterpret_cast<CallbackData*>(data);
  callback_data->notify();
}

void check_result_callback(CassFuture* future, void* data) {
  CallbackData* callback_data = reinterpret_cast<CallbackData*>(data);

  test_utils::CassResultPtr result(cass_future_get_result(future));

  if (result) {
    boost::unique_lock<boost::mutex> lock(callback_data->mutex);
    callback_data->row_count = cass_result_row_count(result.get());
  }

  callback_data->notify();
}

} // namespace

struct FutureCallbacksTests : public test_utils::MultipleNodesTest {
  FutureCallbacksTests() : test_utils::MultipleNodesTest(1, 0) {
  }
};

BOOST_FIXTURE_TEST_SUITE(future_callbacks, FutureCallbacksTests)

BOOST_AUTO_TEST_CASE(test_connect)
{
  boost::scoped_ptr<CallbackData> callback_data(new CallbackData());

  test_utils::CassFuturePtr future(cass_cluster_connect(cluster));
  cass_future_set_callback(future.get(), check_callback, callback_data.get());

  callback_data->wait();

  BOOST_CHECK(callback_data->was_called);
}

BOOST_AUTO_TEST_CASE(test_close)
{
  boost::scoped_ptr<CallbackData> callback_data(new CallbackData());

  test_utils::CassFuturePtr connect_future(cass_cluster_connect(cluster));
  test_utils::wait_and_check_error(connect_future.get());

  CassSession* session = cass_future_get_session(connect_future.get());

  test_utils::CassFuturePtr close_future(cass_session_close(session));
  cass_future_set_callback(close_future.get(), check_callback, callback_data.get());

  callback_data->wait();

  BOOST_CHECK(callback_data->was_called);
}

BOOST_AUTO_TEST_CASE(test_result)
{
  boost::scoped_ptr<CallbackData> callback_data(new CallbackData());

  test_utils::CassFuturePtr connect_future(cass_cluster_connect(cluster));
  test_utils::wait_and_check_error(connect_future.get());
  test_utils::CassSessionPtr session(cass_future_get_session(connect_future.get()));

  test_utils::CassResultPtr result;

  test_utils::CassStatementPtr statement(cass_statement_new(cass_string_init("SELECT * FROM system.schema_keyspaces"), 0));
  test_utils::CassFuturePtr future(cass_session_execute(session.get(), statement.get()));

  cass_future_set_callback(future.get(), check_result_callback, callback_data.get());

  callback_data->wait();

  BOOST_CHECK(callback_data->was_called);
  BOOST_CHECK(callback_data->row_count > 0);
}

BOOST_AUTO_TEST_CASE(test_after_set)
{
  boost::scoped_ptr<CallbackData> callback_data(new CallbackData());

  test_utils::CassFuturePtr future(cass_cluster_connect(cluster));
  test_utils::wait_and_check_error(future.get());

  cass_future_set_callback(future.get(), check_callback, callback_data.get());

  callback_data->wait();

  BOOST_CHECK(callback_data->was_called);
}

BOOST_AUTO_TEST_SUITE_END()

