#include "gtest/gtest.h"

// TODO(pcm): get rid of this and replace with the type safe plasma event loop
extern "C" {
#include "hiredis/adapters/ae.h"
#include "hiredis/async.h"
#include "hiredis/hiredis.h"
}

#include "ray/gcs/client.h"
#include "ray/gcs/tables.h"

namespace ray {

/* Flush redis. */
static inline void flushall_redis(void) {
  redisContext *context = redisConnect("127.0.0.1", 6379);
  freeReplyObject(redisCommand(context, "FLUSHALL"));
  redisFree(context);
}

class TestGcs : public ::testing::Test {
 public:
  TestGcs() {
    client_ = std::make_shared<gcs::AsyncGcsClient>();
    RAY_CHECK_OK(client_->Connect("127.0.0.1", 6379));

    job_id_ = JobID::from_random();
  }

  virtual ~TestGcs() {
    // Clear all keys in the GCS.
    flushall_redis();
  };

  virtual void Start() = 0;

  virtual void Stop() = 0;

 protected:
  std::shared_ptr<gcs::AsyncGcsClient> client_;
  JobID job_id_;
};

TestGcs *test;

class TestGcsWithAe : public TestGcs {
 public:
  TestGcsWithAe() {
    loop_ = aeCreateEventLoop(1024);
    RAY_CHECK_OK(client_->context()->AttachToEventLoop(loop_));
  }
  ~TestGcsWithAe() override {
    // Destroy the client first since it has a reference to the event loop.
    client_.reset();
    aeDeleteEventLoop(loop_);
  }
  void Start() override { aeMain(loop_); }
  void Stop() override { aeStop(loop_); }

 private:
  aeEventLoop *loop_;
};

class TestGcsWithAsio : public TestGcs {
 public:
  TestGcsWithAsio() : TestGcs(), io_service_(), work_(io_service_) {
    RAY_CHECK_OK(client_->Attach(io_service_));
  }
  ~TestGcsWithAsio() {
    // Destroy the client first since it has a reference to the event loop.
    client_.reset();
  }
  void Start() override { io_service_.run(); }
  void Stop() override { io_service_.stop(); }

 private:
  boost::asio::io_service io_service_;
  // Give the event loop some work so that it's forced to run until Stop() is
  // called.
  boost::asio::io_service::work work_;
};

// Object table callback functions.
void ObjectAdded(gcs::AsyncGcsClient *client, const UniqueID &id,
                 std::shared_ptr<ObjectTableDataT> data) {
  ASSERT_EQ(data->managers, std::vector<std::string>({"A", "B"}));
}

void ObjectLookup(gcs::AsyncGcsClient *client, const UniqueID &id,
            std::shared_ptr<ObjectTableDataT> data) {
  // Check that the object entry was added.
  ASSERT_EQ(data->managers, std::vector<std::string>({"A", "B"}));
  test->Stop();
}

void ObjectLookupFailed(gcs::AsyncGcsClient *client, const UniqueID &id) {
  // Object entry failed.
  RAY_CHECK(false);
  test->Stop();
}

// Heartbeat table callback functions.
void HeartbeatAdded(gcs::AsyncGcsClient *client, const ClientID &id,
                 std::shared_ptr<HeartbeatTableDataT> data) {
  std::vector<std::string> labels({"CPU", "GPU"});
  std::vector<double> available_capacity({1,0});
  std::vector<double> total_capacity({2,2});
  ASSERT_EQ(data->client_id, id.hex());
  ASSERT_EQ(data->resources_available_label, labels);
  ASSERT_EQ(data->resources_total_label, labels);
  ASSERT_EQ(data->resources_available_capacity, available_capacity);
  ASSERT_EQ(data->resources_total_capacity, total_capacity);
}

void HeartbeatLookup(gcs::AsyncGcsClient *client, const ClientID &id,
                  std::shared_ptr<HeartbeatTableDataT> data) {
  // Check that the object entry was added.
  HeartbeatAdded(client, id, data);
  test->Stop();
}

void HeartbeatLookupFailed(gcs::AsyncGcsClient *client, const ClientID &id) {
  // Object entry failed.
  RAY_CHECK(false);
  test->Stop();
}

void TestObjectTable(const JobID &job_id, std::shared_ptr<gcs::AsyncGcsClient> client) {
  auto data = std::make_shared<ObjectTableDataT>();
  data->managers.push_back("A");
  data->managers.push_back("B");
  ObjectID object_id = ObjectID::from_random();
  RAY_CHECK_OK(client->object_table().Add(job_id, object_id, data, &ObjectAdded));
  RAY_CHECK_OK(client->object_table().Lookup(job_id, object_id, &ObjectLookup, &ObjectLookupFailed));
  // Run the event loop. The loop will only stop if the ObjectLookup callback is
  // called (or an assertion failure).
  test->Start();
}

void TestHeartbeatTable(const JobID &job_id, std::shared_ptr<gcs::AsyncGcsClient> client) {
  auto data = std::make_shared<HeartbeatTableDataT>();
  auto client_id = ClientID::from_random();
  data->client_id = client_id.hex();
  std::vector<std::string> labels({"CPU", "GPU"});
  std::unordered_map<std::string, double> resources_available({{"CPU", 1}, {"GPU", 0}});
  std::unordered_map<std::string, double> resources_total({{"CPU", 2}, {"GPU", 2}});
  for (const auto &resource_label : labels) {
    data->resources_available_label.push_back(resource_label);
    data->resources_available_capacity.push_back(resources_available[resource_label]);
    data->resources_total_label.push_back(resource_label);
    data->resources_total_capacity.push_back(resources_total[resource_label]);

  }

  RAY_CHECK_OK(client->heartbeat_table()
                   .Add(job_id, client_id, data, &HeartbeatAdded));
  RAY_CHECK_OK(client->heartbeat_table()
                   .Lookup(job_id, client_id, HeartbeatLookup, HeartbeatLookupFailed));
  // Run the event loop.
  test->Start();
}

TEST_F(TestGcsWithAe, TestObjectTable) {
  test = this;
  TestObjectTable(job_id_, client_);
}

TEST_F(TestGcsWithAsio, TestObjectTable) {
  test = this;
  TestObjectTable(job_id_, client_);
}

TEST_F(TestGcsWithAsio, TestHeartbeatTable) {
  test = this;
  TestHeartbeatTable(job_id_, client_);
}

TEST_F(TestGcsWithAe, TestHeartbeatTable) {
  test = this;
  TestHeartbeatTable(job_id_, client_);
}

// Task table callbacks.
void TaskAdded(gcs::AsyncGcsClient *client, const TaskID &id,
               std::shared_ptr<TaskTableDataT> data) {
  ASSERT_EQ(data->scheduling_state, SchedulingState_SCHEDULED);
}

void TaskLookup(gcs::AsyncGcsClient *client, const TaskID &id,
                std::shared_ptr<TaskTableDataT> data) {
  ASSERT_EQ(data->scheduling_state, SchedulingState_SCHEDULED);
}

void TaskLookupFailure(gcs::AsyncGcsClient *client, const TaskID &id) {
  RAY_CHECK(false);
}

void TaskLookupAfterUpdate(gcs::AsyncGcsClient *client, const TaskID &id,
                           std::shared_ptr<TaskTableDataT> data) {
  ASSERT_EQ(data->scheduling_state, SchedulingState_LOST);
  test->Stop();
}

void TaskLookupAfterUpdateFailure(gcs::AsyncGcsClient *client,
                                  const TaskID &id) {
  RAY_CHECK(false);
  test->Stop();
}

void TaskUpdateCallback(gcs::AsyncGcsClient *client, const TaskID &task_id,
                        const TaskTableDataT &task, bool updated) {
  RAY_CHECK_OK(client->legacy_task_table().Lookup(
      DriverID::nil(), task_id, &TaskLookupAfterUpdate, &TaskLookupAfterUpdateFailure));
}

void TestTaskTable(const JobID &job_id, std::shared_ptr<gcs::AsyncGcsClient> client) {
  auto data = std::make_shared<TaskTableDataT>();
  data->scheduling_state = SchedulingState_SCHEDULED;
  ClientID local_scheduler_id = ClientID::from_binary("abcdefghijklmnopqrst");
  data->scheduler_id = local_scheduler_id.binary();
  TaskID task_id = TaskID::from_random();
  RAY_CHECK_OK(client->legacy_task_table().Add(job_id, task_id, data, &TaskAdded));
  RAY_CHECK_OK(
      client->legacy_task_table().Lookup(job_id, task_id, &TaskLookup, &TaskLookupFailure));
  auto update = std::make_shared<TaskTableTestAndUpdateT>();
  update->test_scheduler_id = local_scheduler_id.binary();
  update->test_state_bitmask = SchedulingState_SCHEDULED;
  update->update_state = SchedulingState_LOST;
  // After test-and-setting, the callback will lookup the current state of the
  // task.
  RAY_CHECK_OK(
      client->legacy_task_table().TestAndUpdate(job_id, task_id, update, &TaskUpdateCallback));
  // Run the event loop. The loop will only stop if the lookup after the
  // test-and-set succeeds (or an assertion failure).
  test->Start();
}

TEST_F(TestGcsWithAe, TestTaskTable) {
  test = this;
  TestTaskTable(job_id_, client_);
}

TEST_F(TestGcsWithAsio, TestTaskTable) {
  test = this;
  TestTaskTable(job_id_, client_);
}

void ObjectTableSubscribed(gcs::AsyncGcsClient *client, const UniqueID &id,
                           std::shared_ptr<ObjectTableDataT> data) {
  test->Stop();
}

void TestSubscribeAll(const JobID &job_id, std::shared_ptr<gcs::AsyncGcsClient> client) {
  // Subscribe to all object table notifications. The registered callback for
  // notifications will check whether the object below is added.
  RAY_CHECK_OK(client->object_table().Subscribe(job_id, ClientID::nil(), &ObjectLookup,
                                                &ObjectTableSubscribed));
  // Run the event loop. The loop will only stop if the subscription succeeds.
  test->Start();

  // We have subscribed. Add an object table entry.
  auto data = std::make_shared<ObjectTableDataT>();
  data->managers.push_back("A");
  data->managers.push_back("B");
  ObjectID object_id = ObjectID::from_random();
  RAY_CHECK_OK(client->object_table().Add(job_id, object_id, data, &ObjectAdded));
  // Run the event loop. The loop will only stop if the registered subscription
  // callback is called (or an assertion failure).
  test->Start();
}

TEST_F(TestGcsWithAe, TestSubscribeAll) {
  test = this;
  TestSubscribeAll(job_id_, client_);
}

TEST_F(TestGcsWithAsio, TestSubscribeAll) {
  test = this;
  TestSubscribeAll(job_id_, client_);
}

void ClientTableNotification(gcs::AsyncGcsClient *client, const UniqueID &id,
                             std::shared_ptr<ClientTableDataT> data, bool is_insertion) {
  ClientID added_id = client->client_table().GetLocalClientId();
  ASSERT_EQ(ClientID::from_binary(data->client_id), added_id);
  ASSERT_EQ(data->is_insertion, is_insertion);
  ASSERT_FALSE(data->node_manager_address.empty());

  auto cached_client = client->client_table().GetClient(added_id);
  ASSERT_EQ(ClientID::from_binary(cached_client.client_id), added_id);
  ASSERT_EQ(cached_client.is_insertion, is_insertion);
}

void TestClientTableConnect(const JobID &job_id,
                            std::shared_ptr<gcs::AsyncGcsClient> client) {
  // Register callbacks for when a client gets added and removed. The latter
  // event will stop the event loop.
  client->client_table().RegisterClientAddedCallback(
      [](gcs::AsyncGcsClient *client, const UniqueID &id,
         std::shared_ptr<ClientTableDataT> data) {
        ClientTableNotification(client, id, data, true);
        test->Stop();
      });

  // Connect and disconnect to client table. We should receive notifications
  // for the addition and removal of our own entry.
  ClientTableDataT local_client_info = client->client_table().GetLocalClient();
  local_client_info.node_manager_address = "127.0.0.1";
  local_client_info.node_manager_port = 0;
  local_client_info.object_manager_port = 0;
  RAY_CHECK_OK(client->client_table().Connect(local_client_info));
  test->Start();
}

TEST_F(TestGcsWithAsio, TestClientTableConnect) {
  test = this;
  TestClientTableConnect(job_id_, client_);
}

void TestClientTableDisconnect(const JobID &job_id,
                               std::shared_ptr<gcs::AsyncGcsClient> client) {
  // Register callbacks for when a client gets added and removed. The latter
  // event will stop the event loop.
  client->client_table().RegisterClientAddedCallback(
      [](gcs::AsyncGcsClient *client, const UniqueID &id,
         std::shared_ptr<ClientTableDataT> data) {
        ClientTableNotification(client, id, data, true);
      });
  client->client_table().RegisterClientRemovedCallback(
      [](gcs::AsyncGcsClient *client, const UniqueID &id,
         std::shared_ptr<ClientTableDataT> data) {
        ClientTableNotification(client, id, data, false);
        test->Stop();
      });
  // Connect and disconnect to client table. We should receive notifications
  // for the addition and removal of our own entry.
  ClientTableDataT local_client_info = client->client_table().GetLocalClient();
  local_client_info.node_manager_address = "127.0.0.1";
  local_client_info.node_manager_port = 0;
  local_client_info.object_manager_port = 0;
  RAY_CHECK_OK(client->client_table().Connect(local_client_info));
  RAY_CHECK_OK(client->client_table().Disconnect());
  test->Start();
}

TEST_F(TestGcsWithAsio, TestClientTableDisconnect) {
  test = this;
  TestClientTableDisconnect(job_id_, client_);
}

}  // namespace
