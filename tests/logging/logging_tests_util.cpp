#include "gtest/gtest.h"
#include "harness.h"

#include <thread>
#include <getopt.h>

#include "logging/logging_tests_util.h"

#include "backend/bridge/ddl/ddl_database.h"
#include "backend/concurrency/transaction_manager.h"
#include "backend/common/value_factory.h"
#include "backend/storage/table_factory.h"
#include "backend/storage/database.h"
#include "backend/storage/data_table.h"
#include "backend/storage/tuple.h"
#include "backend/storage/tile_group.h"
#include "backend/logging/log_manager.h"
#include "backend/logging/records/tuple_record.h"
#include "backend/logging/records/transaction_record.h"

namespace peloton {
namespace test {

//===--------------------------------------------------------------------===//
// PREPARE LOG FILE
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// 1. Standby -- Bootstrap
// 2. Recovery -- Optional
// 3. Logging -- Collect data and flush when commit
// 4. Terminate -- Collect any remaining data and flush
// 5. Sleep -- Disconnect backend loggers and frontend logger from manager
//===--------------------------------------------------------------------===//

#define LOGGING_TESTS_DATABASE_OID 20000
#define LOGGING_TESTS_TABLE_OID    10000

// configuration for testing
LoggingTestsUtil::logging_test_configuration state;

/**
 * @brief writing a simple log file 
 */
bool LoggingTestsUtil::PrepareLogFile(LoggingType logging_type, std::string file_name){
  std::string file_path = state.file_dir + file_name;

  std::ifstream log_file(file_path);

  // Reset the log file if exists
  if( log_file.good() ){
    EXPECT_TRUE(std::remove(file_path.c_str()) == 0 );
  }
  log_file.close();

  // start a thread for logging
  auto& log_manager = logging::LogManager::GetInstance();

  if( log_manager.ActiveFrontendLoggerCount() > 0){
    LOG_ERROR("another logging thread is running now");
    return false;
  }

  // set log file and logging type
  log_manager.SetLogFileName(file_path);

  // start off the frontend logger of appropriate type in STANDBY mode
  std::thread thread(&logging::LogManager::StartStandbyMode,
                     &log_manager,
                     logging_type);

  // wait for the frontend logger to enter STANDBY mode
  log_manager.WaitForMode(LOGGING_STATUS_TYPE_STANDBY, true, logging_type);

  // suspend final step in transaction commit,
  // so that it only get committed during recovery
  if (state.redo_all) {
    log_manager.SetTestRedoAllLogs(logging_type, true);
  }

  // STANDBY -> RECOVERY mode
  log_manager.StartRecoveryMode(logging_type);

  // Wait for the frontend logger to enter LOGGING mode
  log_manager.WaitForMode(LOGGING_STATUS_TYPE_LOGGING, true, logging_type);

  // Build the log
  LoggingTestsUtil::BuildLog(logging_type,
                             LOGGING_TESTS_DATABASE_OID,
                             LOGGING_TESTS_TABLE_OID);

  //  Wait for the mode transition :: LOGGING -> TERMINATE -> SLEEP
  if(log_manager.EndLogging(logging_type)){
    thread.join();
    return true;
  }

  LOG_ERROR("Failed to terminate logging thread");
  return false;
}

//===--------------------------------------------------------------------===//
// CHECK RECOVERY
//===--------------------------------------------------------------------===//

void LoggingTestsUtil::ResetSystem(){
  // Initialize oid since we assume that we restart the system
  auto &manager = catalog::Manager::GetInstance();
  manager.SetNextOid(0);
  manager.ClearTileGroup();

  auto &txn_manager = concurrency::TransactionManager::GetInstance();
  txn_manager.ResetStates();
}

/**
 * @brief recover the database and check the tuples
 */
void LoggingTestsUtil::CheckRecovery(LoggingType logging_type, std::string file_name){
  std::string file_path = state.file_dir + file_name;

  std::ifstream log_file(file_path);

  // Reset the log file if exists
  EXPECT_TRUE(log_file.good());
  log_file.close();

  LoggingTestsUtil::CreateDatabaseAndTable(LOGGING_TESTS_DATABASE_OID, LOGGING_TESTS_TABLE_OID);

  // start a thread for logging
  auto& log_manager = logging::LogManager::GetInstance();
  if( log_manager.ActiveFrontendLoggerCount() > 0){
    LOG_ERROR("another logging thread is running now");
    return;
  }

  // set log file and logging type
  log_manager.SetLogFileName(file_path);

  // start off the frontend logger of appropriate type in STANDBY mode
  std::thread thread(&logging::LogManager::StartStandbyMode, 
                     &log_manager,
                     logging_type);

  // wait for the frontend logger to enter STANDBY mode
  log_manager.WaitForMode(LOGGING_STATUS_TYPE_STANDBY, true, logging_type);

  // always enable commit when testing recovery
  if (state.redo_all) {
    log_manager.SetTestRedoAllLogs(logging_type, true);
  }

  // STANDBY -> RECOVERY mode
  log_manager.StartRecoveryMode(logging_type);

  // Wait for the frontend logger to enter LOGGING mode after recovery
  log_manager.WaitForMode(LOGGING_STATUS_TYPE_LOGGING, true, logging_type);

  // Check the tuple count if needed
  if (state.check_tuple_count) {
    oid_t per_thread_expected = state.tuple_count - 1;
    oid_t total_expected =  per_thread_expected * state.backend_count;

    LoggingTestsUtil::CheckTupleCount(LOGGING_TESTS_DATABASE_OID,
                                      LOGGING_TESTS_TABLE_OID,
                                      total_expected);
  }

  // Check the next oid
  //LoggingTestsUtil::CheckNextOid();

  if( log_manager.EndLogging(logging_type) ){
    thread.join();
  }else{
    LOG_ERROR("Failed to terminate logging thread");
  }
  LoggingTestsUtil::DropDatabaseAndTable(LOGGING_TESTS_DATABASE_OID, LOGGING_TESTS_TABLE_OID);
}

void LoggingTestsUtil::CheckTupleCount(oid_t db_oid, oid_t table_oid, oid_t expected){

  auto &manager = catalog::Manager::GetInstance();
  storage::Database *db = manager.GetDatabaseWithOid(db_oid);
  auto table = db->GetTableWithOid(table_oid);

  oid_t tile_group_count = table->GetTileGroupCount();
  oid_t active_tuple_count = 0;
  for (oid_t tile_group_itr = 0; tile_group_itr < tile_group_count;
      tile_group_itr++) {
    auto tile_group = table->GetTileGroup(tile_group_itr);
    active_tuple_count += tile_group->GetActiveTupleCount();
  }

  // check # of active tuples
  EXPECT_EQ(expected, active_tuple_count);
}

//===--------------------------------------------------------------------===//
// WRITING LOG RECORD
//===--------------------------------------------------------------------===//

void LoggingTestsUtil::BuildLog(LoggingType logging_type,
                                oid_t db_oid, oid_t table_oid){

  // Create db
  CreateDatabase(db_oid);
  auto &manager = catalog::Manager::GetInstance();
  storage::Database *db = manager.GetDatabaseWithOid(db_oid);

  // Create table, drop it and create again
  // so that table can have a newly added tile group and
  // not just the default tile group
  storage::DataTable* table = CreateUserTable(db_oid, table_oid);
  db->AddTable(table);

  // Execute the workload to build the log
  LaunchParallelTest(state.backend_count, RunBackends, logging_type, table);

  // Check the tuple count if needed
  if (state.check_tuple_count) {
    oid_t per_thread_expected = state.tuple_count - 1;
    oid_t total_expected =  per_thread_expected * state.backend_count;

    LoggingTestsUtil::CheckTupleCount(db_oid, table_oid, total_expected);
  }

  // We can only drop the table in case of ARIES
  if(logging_type == LOGGING_TYPE_ARIES){
    db->DropTableWithOid(table_oid);
    DropDatabase(db_oid);
  }
}


void LoggingTestsUtil::RunBackends(LoggingType logging_type,
                                   storage::DataTable* table){

  bool commit = true;
  auto testing_pool = TestingHarness::GetInstance().GetTestingPool();

  // Insert tuples
  auto locations = InsertTuples(logging_type, table, testing_pool, commit);

  // Update tuples
  locations = UpdateTuples(logging_type, table, locations, testing_pool, commit);

  // Delete tuples
  DeleteTuples(logging_type, table, locations, commit);

  // Remove the backend logger after flushing out all the changes
  auto& log_manager = logging::LogManager::GetInstance();
  if(log_manager.IsInLoggingMode(logging_type)){
    auto logger = log_manager.GetBackendLogger(logging_type);

    // Wait until frontend logger collects the data
    logger->WaitForFlushing();

    log_manager.RemoveBackendLogger(logger);
  }

}

// Do insert and create insert tuple log records
std::vector<ItemPointer> LoggingTestsUtil::InsertTuples(LoggingType logging_type,
                                                        storage::DataTable* table,
                                                        VarlenPool *pool,
                                                        bool committed){
  std::vector<ItemPointer> locations;

  // Create Tuples
  auto tuples = CreateTuples(table->GetSchema(), state.tuple_count, pool);

  auto &txn_manager = concurrency::TransactionManager::GetInstance();

  for( auto tuple : tuples){
    auto txn = txn_manager.BeginTransaction();
    ItemPointer location = table->InsertTuple(txn, tuple);
    if (location.block == INVALID_OID) {
      txn->SetResult(Result::RESULT_FAILURE);
      std::cout << "Insert failed \n";
      exit(EXIT_FAILURE);
    }

    txn->RecordInsert(location);

    locations.push_back(location);

    // Logging 
    {
      auto& log_manager = logging::LogManager::GetInstance();

      if(log_manager.IsInLoggingMode(logging_type)){
        auto logger = log_manager.GetBackendLogger(logging_type);
        auto record = logger->GetTupleRecord(LOGRECORD_TYPE_TUPLE_INSERT,
                                             txn->GetTransactionId(), 
                                             table->GetOid(),
                                             location,
                                             INVALID_ITEMPOINTER,
                                             tuple,
                                             LOGGING_TESTS_DATABASE_OID);
        logger->Log(record);

      }
    }

    // commit or abort as required
    if(committed){
      txn_manager.CommitTransaction();
    } else{
      txn_manager.AbortTransaction();
    }
  }

  // Clean up data
  for( auto tuple : tuples){
    delete tuple;
  }

  return locations;
}

void LoggingTestsUtil::DeleteTuples(LoggingType logging_type,
                                    storage::DataTable* table,
                                    const std::vector<ItemPointer>& locations,
                                    bool committed){

  for(auto delete_location : locations) {

    auto &txn_manager = concurrency::TransactionManager::GetInstance();
    auto txn = txn_manager.BeginTransaction();

    bool status = table->DeleteTuple(txn, delete_location);
    if (status == false) {
      txn->SetResult(Result::RESULT_FAILURE);
      std::cout << "Delete failed \n";
      exit(EXIT_FAILURE);
    }

    txn->RecordDelete(delete_location);

    // Logging
    {
      auto& log_manager = logging::LogManager::GetInstance();

      if(log_manager.IsInLoggingMode(logging_type)){
        auto logger = log_manager.GetBackendLogger(logging_type);
        auto record = logger->GetTupleRecord(LOGRECORD_TYPE_TUPLE_DELETE,
                                             txn->GetTransactionId(),
                                             table->GetOid(),
                                             INVALID_ITEMPOINTER,
                                             delete_location,
                                             nullptr,
                                             LOGGING_TESTS_DATABASE_OID);
        logger->Log(record);
      }
    }

    if(committed){
      txn_manager.CommitTransaction();
    }else{
      txn_manager.AbortTransaction();
    }

  }

}

std::vector<ItemPointer> LoggingTestsUtil::UpdateTuples(LoggingType logging_type,
                                                        storage::DataTable* table,
                                                        const std::vector<ItemPointer>& deleted_locations,
                                                        VarlenPool *pool,
                                                        bool committed){

  // Inserted locations
  std::vector<ItemPointer> inserted_locations;

  // Create Tuples
  auto tuple_count = deleted_locations.size();
  auto tuples = CreateTuples(table->GetSchema(), tuple_count, pool);

  size_t tuple_itr = 0;
  for(auto delete_location : deleted_locations) {

    auto tuple = tuples[tuple_itr];
    tuple_itr++;

    auto &txn_manager = concurrency::TransactionManager::GetInstance();
    auto txn = txn_manager.BeginTransaction();

    bool status = table->DeleteTuple(txn, delete_location);
    if (status == false) {
      txn->SetResult(Result::RESULT_FAILURE);
      std::cout << "Delete failed \n";
      exit(EXIT_FAILURE);
    }

    txn->RecordDelete(delete_location);

    ItemPointer insert_location = table->InsertTuple(txn, tuple);
    if (insert_location.block == INVALID_OID) {
      txn->SetResult(Result::RESULT_FAILURE);
      std::cout << "Insert failed \n";
      exit(EXIT_FAILURE);
    }
    txn->RecordInsert(insert_location);

    inserted_locations.push_back(insert_location);

    // Logging
    {
      auto& log_manager = logging::LogManager::GetInstance();
      if(log_manager.IsInLoggingMode(logging_type)){
        auto logger = log_manager.GetBackendLogger(logging_type);
        auto record = logger->GetTupleRecord(LOGRECORD_TYPE_TUPLE_UPDATE,
                                             txn->GetTransactionId(),
                                             table->GetOid(),
                                             insert_location,
                                             delete_location,
                                             tuple,
                                             LOGGING_TESTS_DATABASE_OID);
        logger->Log(record);
      }
    }


    if(committed){
      txn_manager.CommitTransaction();
    } else{
      txn_manager.AbortTransaction();
    }
  }

  // Clean up data
  for( auto tuple : tuples){
    delete tuple;
  }

  return inserted_locations;
}

//===--------------------------------------------------------------------===//
// Utility functions
//===--------------------------------------------------------------------===//

void LoggingTestsUtil::CreateDatabaseAndTable(oid_t db_oid, oid_t table_oid){

  // Create database and attach a table
  bridge::DDLDatabase::CreateDatabase(db_oid);
  auto &manager = catalog::Manager::GetInstance();
  storage::Database *db = manager.GetDatabaseWithOid(db_oid);

  auto table = CreateUserTable(db_oid, table_oid);

  db->AddTable(table);
}

storage::DataTable* LoggingTestsUtil::CreateUserTable(oid_t db_oid, oid_t table_oid){

  auto column_infos = LoggingTestsUtil::CreateSchema();

  bool own_schema = true;
  bool adapt_table = false;
  const int tuples_per_tilegroup_count = 10;

  // Construct our schema from vector of ColumnInfo
  auto schema = new catalog::Schema(column_infos);
  storage::DataTable *table = storage::TableFactory::GetDataTable(db_oid, table_oid,
                                                                  schema,
                                                                  "USERTABLE",
                                                                  tuples_per_tilegroup_count,
                                                                  own_schema,
                                                                  adapt_table);

  return table;
}

void LoggingTestsUtil::CreateDatabase(oid_t db_oid){
  // Create Database
  bridge::DDLDatabase::CreateDatabase(db_oid);
}

std::vector<catalog::Column> LoggingTestsUtil::CreateSchema() {
  // Columns
  std::vector<catalog::Column> columns;
  const size_t field_length = 100;

  // User Id
  catalog::Column user_id(VALUE_TYPE_INTEGER,
                          GetTypeSize(VALUE_TYPE_INTEGER),
                          "YCSB_KEY",
                          true);

  columns.push_back(user_id);

  // Field
  for(oid_t col_itr = 0 ; col_itr < state.column_count ; col_itr++) {
    catalog::Column field(VALUE_TYPE_VARCHAR,
                          field_length,
                          "FIELD" + std::to_string(col_itr),
                          false);

    columns.push_back(field);
  }

  return columns;
}

std::vector<storage::Tuple*> LoggingTestsUtil::CreateTuples(catalog::Schema* schema, oid_t num_of_tuples, VarlenPool *pool) {

  std::vector<storage::Tuple*> tuples;
  const bool allocate = true;

  for (oid_t tuple_itr = 0; tuple_itr < num_of_tuples; tuple_itr++) {
    // Build tuple
    storage::Tuple *tuple = new storage::Tuple(schema, allocate);

    Value user_id_value = ValueFactory::GetIntegerValue(tuple_itr);
    tuple->SetValue(0, user_id_value, nullptr);

    for(oid_t col_itr = 1 ; col_itr < state.column_count; col_itr++) {
      Value field_value = ValueFactory::GetStringValue(std::to_string(tuple_itr), pool);
      tuple->SetValue(col_itr, field_value, pool);
    }

    tuples.push_back(tuple);
  }

  return tuples;
}

void LoggingTestsUtil::DropDatabaseAndTable(oid_t db_oid, oid_t table_oid){
  auto &manager = catalog::Manager::GetInstance();

  storage::Database *db = manager.GetDatabaseWithOid(db_oid);
  db->DropTableWithOid(table_oid);

  bridge::DDLDatabase::DropDatabase(db_oid);
}

void LoggingTestsUtil::DropDatabase(oid_t db_oid){
  bridge::DDLDatabase::DropDatabase(db_oid);
}

//===--------------------------------------------------------------------===//
// Configuration
//===--------------------------------------------------------------------===//

static void Usage(FILE *out) {
  fprintf(out, "Command line options :  hyadapt <options> \n"
          "   -h --help              :  Print help message \n"
          "   -l --logging-type      :  Logging type \n"
          "   -t --tuple-count       :  Tuple count \n"
          "   -b --backend-count     :  Backend count \n"
          "   -z --column_count      :  Column count \n"
          "   -c --check-tuple-count :  Check tuple count \n"
          "   -r --redo-all-logs     :  Redo all logs \n"
          "   -d --dir               :  log file dir \n"
  );
  exit(EXIT_FAILURE);
}

static struct option opts[] = {
    { "logging-type", optional_argument, NULL, 'l' },
    { "tuple-count", optional_argument, NULL, 't' },
    { "backend-count", optional_argument, NULL, 'b' },
    { "tuple-size", optional_argument, NULL, 'z' },
    { "check-tuple-count", optional_argument, NULL, 'c' },
    { "redo-all-logs", optional_argument, NULL, 'r' },
    { "dir", optional_argument, NULL, 'd' },
    { NULL, 0, NULL, 0 }
};

static void PrintConfiguration(){
  int width = 25;

  std::cout << std::setw(width) << std::left
      << "logging_type " << " : ";

  if(state.logging_type == LOGGING_TYPE_ARIES)
    std::cout << "ARIES" << std::endl;
  else if(state.logging_type == LOGGING_TYPE_PELOTON)
    std::cout << "PELOTON" << std::endl;
  else {
    std::cout << "INVALID" << std::endl;
    exit(EXIT_FAILURE);
  }

  std::cout << std::setw(width) << std::left
      << "tuple_count " << " : " << state.tuple_count << std::endl;
  std::cout << std::setw(width) << std::left
      << "backend_count " << " : " << state.backend_count << std::endl;
  std::cout << std::setw(width) << std::left
      << "column_count " << " : " << state.column_count << std::endl;
  std::cout << std::setw(width) << std::left
      << "check_tuple_count " << " : " << state.check_tuple_count << std::endl;
  std::cout << std::setw(width) << std::left
      << "redo_all_logs " << " : " << state.redo_all << std::endl;
  std::cout << std::setw(width) << std::left
      << "dir " << " : " << state.file_dir << std::endl;
}

void LoggingTestsUtil::ParseArguments(int argc, char* argv[]) {

  // Default Values
  state.tuple_count = 100;

  state.logging_type = LOGGING_TYPE_ARIES;
  state.backend_count = 2;

  state.column_count = 10;

  state.check_tuple_count = false;
  state.redo_all = false;

  state.file_dir = "/tmp/";

  // Parse args
  while (1) {
    int idx = 0;
    int c = getopt_long(argc, argv, "ahl:t:b:z:c:r:d:", opts,
                        &idx);

    if (c == -1)
      break;

    switch (c) {
      case 'l':
        state.logging_type = (LoggingType) atoi(optarg);
        break;
      case 't':
        state.tuple_count  = atoi(optarg);
        break;
      case 'b':
        state.backend_count  = atoi(optarg);
        break;
      case 'z':
        state.column_count  = atoi(optarg);
        break;
      case 'c':
        state.check_tuple_count  = atoi(optarg);
        break;
      case 'r':
        state.redo_all  = atoi(optarg);
        break;
      case 'd':
        state.file_dir = optarg;
        break;

      case 'h':
        Usage(stderr);
        break;

      default:
        fprintf(stderr, "\nUnknown option: -%c-\n", c);
        Usage(stderr);
    }
  }

  PrintConfiguration();

}


}  // End test namespace
}  // End peloton namespace
