////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "MMFilesV8Functions.h"
#include "Basics/Exceptions.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ServerState.h"
#include "MMFiles/MMFilesCollection.h"
#include "MMFiles/MMFilesEngine.h"
#include "MMFiles/MMFilesLogfileManager.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/V8Context.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-vocbaseprivate.h"
#include "VocBase/LogicalCollection.h"

#include <v8.h>

using namespace arangodb;

/// @brief rotate the active journal of the collection
static void JS_RotateVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (ServerState::instance()->isCoordinator()) {
    // renaming a collection in a cluster is unsupported
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_CLUSTER_UNSUPPORTED);
  }

  PREVENT_EMBEDDED_TRANSACTION();

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
  
  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  SingleCollectionTransaction trx(
      transaction::V8Context::Create(collection->vocbase(), true),
      collection->cid(), AccessMode::Type::READ);

  Result res = trx.begin();
  
  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  res = static_cast<MMFilesCollection*>(collection->getPhysical())->rotateActiveJournal();

  trx.finish(res);

  if (!res.ok()) {
    res.reset(res.errorNumber(), std::string("could not rotate journal: ") + res.errorMessage());
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

/// @brief returns information about the datafiles
/// the collection must be unloaded.
static void JS_DatafilesVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  // TODO: move this into engine
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  TRI_vocbase_col_status_e status = collection->getStatusLocked();
  if (status != TRI_VOC_COL_STATUS_UNLOADED &&
      status != TRI_VOC_COL_STATUS_CORRUPTED) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_UNLOADED);
  }

  MMFilesEngineCollectionFiles structure = dynamic_cast<MMFilesEngine*>(engine)->scanCollectionDirectory(collection->getPhysical()->path());

  // build result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  // journals
  v8::Handle<v8::Array> journals = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("journals"), journals);

  uint32_t i = 0;
  for (auto& it : structure.journals) {
    journals->Set(i++, TRI_V8_STD_STRING(it));
  }

  // compactors
  v8::Handle<v8::Array> compactors = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("compactors"), compactors);

  i = 0;
  for (auto& it : structure.compactors) {
    compactors->Set(i++, TRI_V8_STD_STRING(it));
  }

  // datafiles
  v8::Handle<v8::Array> datafiles = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("datafiles"), datafiles);

  i = 0;
  for (auto& it : structure.datafiles) {
    datafiles->Set(i++, TRI_V8_STD_STRING(it));
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

/// @brief returns information about the datafiles
/// Returns information about the datafiles. The collection must be unloaded.
static void JS_DatafileScanVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("datafileScan(<path>)");
  }

  std::string path = TRI_ObjectToString(args[0]);

  v8::Handle<v8::Object> result;
  {
    // TODO Check with JAN Okay to just remove the lock?
    // READ_LOCKER(readLocker, collection->_lock);

    TRI_vocbase_col_status_e status = collection->getStatusLocked();
    if (status != TRI_VOC_COL_STATUS_UNLOADED &&
        status != TRI_VOC_COL_STATUS_CORRUPTED) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_UNLOADED);
    }

    DatafileScan scan = MMFilesDatafile::scan(path);

    // build result
    result = v8::Object::New(isolate);

    result->Set(TRI_V8_ASCII_STRING("currentSize"),
                v8::Number::New(isolate, scan.currentSize));
    result->Set(TRI_V8_ASCII_STRING("maximalSize"),
                v8::Number::New(isolate, scan.maximalSize));
    result->Set(TRI_V8_ASCII_STRING("endPosition"),
                v8::Number::New(isolate, scan.endPosition));
    result->Set(TRI_V8_ASCII_STRING("numberMarkers"),
                v8::Number::New(isolate, scan.numberMarkers));
    result->Set(TRI_V8_ASCII_STRING("status"),
                v8::Number::New(isolate, scan.status));
    result->Set(TRI_V8_ASCII_STRING("isSealed"),
                v8::Boolean::New(isolate, scan.isSealed));

    v8::Handle<v8::Array> entries = v8::Array::New(isolate);
    result->Set(TRI_V8_ASCII_STRING("entries"), entries);

    uint32_t i = 0;
    for (auto const& entry : scan.entries) {
      v8::Handle<v8::Object> o = v8::Object::New(isolate);

      o->Set(TRI_V8_ASCII_STRING("position"),
             v8::Number::New(isolate, entry.position));
      o->Set(TRI_V8_ASCII_STRING("size"),
             v8::Number::New(isolate, entry.size));
      o->Set(TRI_V8_ASCII_STRING("realSize"),
             v8::Number::New(isolate, entry.realSize));
      o->Set(TRI_V8_ASCII_STRING("tick"), TRI_V8UInt64String<TRI_voc_tick_t>(isolate, entry.tick));
      o->Set(TRI_V8_ASCII_STRING("type"),
             v8::Number::New(isolate, static_cast<int>(entry.type)));
      o->Set(TRI_V8_ASCII_STRING("status"),
             v8::Number::New(isolate, static_cast<int>(entry.status)));

      if (!entry.key.empty()) {
        o->Set(TRI_V8_ASCII_STRING("key"), TRI_V8_STD_STRING(entry.key));
      }

      if (entry.typeName != nullptr) {
        o->Set(TRI_V8_ASCII_STRING("typeName"),
               TRI_V8_ASCII_STRING(entry.typeName));
      }

      if (!entry.diagnosis.empty()) {
        o->Set(TRI_V8_ASCII_STRING("diagnosis"),
               TRI_V8_STD_STRING(entry.diagnosis));
      }

      entries->Set(i++, o);
    }
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

/// @brief tries to repair a datafile
static void JS_TryRepairDatafileVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("tryRepairDatafile(<datafile>)");
  }

  std::string path = TRI_ObjectToString(args[0]);

  TRI_vocbase_col_status_e status = collection->getStatusLocked();
  if (status != TRI_VOC_COL_STATUS_UNLOADED &&
      status != TRI_VOC_COL_STATUS_CORRUPTED) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_UNLOADED);
  }

  bool result = MMFilesDatafile::tryRepair(path);

  if (result) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

/// @brief truncates a datafile
static void JS_TruncateDatafileVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("truncateDatafile(<datafile>, <size>)");
  }

  std::string path = TRI_ObjectToString(args[0]);
  size_t size = (size_t)TRI_ObjectToInt64(args[1]);

  TRI_vocbase_col_status_e status = collection->getStatusLocked();

  if (status != TRI_VOC_COL_STATUS_UNLOADED &&
      status != TRI_VOC_COL_STATUS_CORRUPTED) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_UNLOADED);
  }

  int res = MMFilesDatafile::truncate(path, static_cast<TRI_voc_size_t>(size));

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, "cannot truncate datafile");
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

static void JS_PropertiesWal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 1 || (args.Length() == 1 && !args[0]->IsObject())) {
    TRI_V8_THROW_EXCEPTION_USAGE("properties(<object>)");
  }

  auto l = MMFilesLogfileManager::instance();

  if (args.Length() == 1) {
    // set the properties
    v8::Handle<v8::Object> object = v8::Handle<v8::Object>::Cast(args[0]);
    if (object->Has(TRI_V8_ASCII_STRING("allowOversizeEntries"))) {
      bool value = TRI_ObjectToBoolean(
          object->Get(TRI_V8_ASCII_STRING("allowOversizeEntries")));
      l->allowOversizeEntries(value);
    }

    if (object->Has(TRI_V8_ASCII_STRING("logfileSize"))) {
      uint32_t value = static_cast<uint32_t>(TRI_ObjectToUInt64(
          object->Get(TRI_V8_ASCII_STRING("logfileSize")), true));
      l->filesize(value);
    }

    if (object->Has(TRI_V8_ASCII_STRING("historicLogfiles"))) {
      uint32_t value = static_cast<uint32_t>(TRI_ObjectToUInt64(
          object->Get(TRI_V8_ASCII_STRING("historicLogfiles")), true));
      l->historicLogfiles(value);
    }

    if (object->Has(TRI_V8_ASCII_STRING("reserveLogfiles"))) {
      uint32_t value = static_cast<uint32_t>(TRI_ObjectToUInt64(
          object->Get(TRI_V8_ASCII_STRING("reserveLogfiles")), true));
      l->reserveLogfiles(value);
    }

    if (object->Has(TRI_V8_ASCII_STRING("throttleWait"))) {
      uint64_t value = TRI_ObjectToUInt64(
          object->Get(TRI_V8_ASCII_STRING("throttleWait")), true);
      l->maxThrottleWait(value);
    }

    if (object->Has(TRI_V8_ASCII_STRING("throttleWhenPending"))) {
      uint64_t value = TRI_ObjectToUInt64(
          object->Get(TRI_V8_ASCII_STRING("throttleWhenPending")), true);
      l->throttleWhenPending(value);
    }
  }

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("allowOversizeEntries"),
              v8::Boolean::New(isolate, l->allowOversizeEntries()));
  result->Set(TRI_V8_ASCII_STRING("logfileSize"),
              v8::Number::New(isolate, l->filesize()));
  result->Set(TRI_V8_ASCII_STRING("historicLogfiles"),
              v8::Number::New(isolate, l->historicLogfiles()));
  result->Set(TRI_V8_ASCII_STRING("reserveLogfiles"),
              v8::Number::New(isolate, l->reserveLogfiles()));
  result->Set(TRI_V8_ASCII_STRING("syncInterval"),
              v8::Number::New(isolate, (double)l->syncInterval()));
  result->Set(TRI_V8_ASCII_STRING("throttleWait"),
              v8::Number::New(isolate, (double)l->maxThrottleWait()));
  result->Set(TRI_V8_ASCII_STRING("throttleWhenPending"),
              v8::Number::New(isolate, (double)l->throttleWhenPending()));

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_FlushWal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  bool waitForSync = false;
  bool waitForCollector = false;
  bool writeShutdownFile = false;

  if (args.Length() > 0) {
    if (args[0]->IsObject()) {
      v8::Handle<v8::Object> obj = args[0]->ToObject();
      if (obj->Has(TRI_V8_ASCII_STRING("waitForSync"))) {
        waitForSync =
            TRI_ObjectToBoolean(obj->Get(TRI_V8_ASCII_STRING("waitForSync")));
      }
      if (obj->Has(TRI_V8_ASCII_STRING("waitForCollector"))) {
        waitForCollector = TRI_ObjectToBoolean(
            obj->Get(TRI_V8_ASCII_STRING("waitForCollector")));
      }
      if (obj->Has(TRI_V8_ASCII_STRING("writeShutdownFile"))) {
        writeShutdownFile = TRI_ObjectToBoolean(
            obj->Get(TRI_V8_ASCII_STRING("writeShutdownFile")));
      }
    } else {
      waitForSync = TRI_ObjectToBoolean(args[0]);

      if (args.Length() > 1) {
        waitForCollector = TRI_ObjectToBoolean(args[1]);

        if (args.Length() > 2) {
          writeShutdownFile = TRI_ObjectToBoolean(args[2]);
        }
      }
    }
  }

  int res;

  if (ServerState::instance()->isCoordinator()) {
    res = flushWalOnAllDBServers(waitForSync, waitForCollector);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
    TRI_V8_RETURN_TRUE();
  }

  res = MMFilesLogfileManager::instance()->flush(
      waitForSync, waitForCollector, writeShutdownFile);

  if (res != TRI_ERROR_NO_ERROR) {
    if (res == TRI_ERROR_LOCK_TIMEOUT) {
      // improved diagnostic message for this special case
      TRI_V8_THROW_EXCEPTION_MESSAGE(res, "timed out waiting for WAL flush operation");
    }
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

/// @brief wait for WAL collector to finish operations for the specified
/// collection
static void JS_WaitCollectorWal(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (ServerState::instance()->isCoordinator()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  TRI_vocbase_t* vocbase = GetContextVocBase(isolate);

  if (vocbase == nullptr) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "WAL_WAITCOLLECTOR(<collection-id>, <timeout>)");
  }

  std::string const name = TRI_ObjectToString(args[0]);

  arangodb::LogicalCollection* col = vocbase->lookupCollection(name);

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
  }

  double timeout = 30.0;
  if (args.Length() > 1) {
    timeout = TRI_ObjectToDouble(args[1]);
  }

  int res = MMFilesLogfileManager::instance()->waitForCollectorQueue(
      col->cid(), timeout);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

/// @brief get information about the currently running transactions
static void JS_TransactionsWal(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto const& info =
      MMFilesLogfileManager::instance()->runningTransactions();

  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  result->ForceSet(
      TRI_V8_ASCII_STRING("runningTransactions"),
      v8::Number::New(isolate, static_cast<double>(std::get<0>(info))));

  // lastCollectedId
  {
    auto value = std::get<1>(info);
    if (value == UINT64_MAX) {
      result->ForceSet(TRI_V8_ASCII_STRING("minLastCollected"),
                       v8::Null(isolate));
    } else {
      result->ForceSet(TRI_V8_ASCII_STRING("minLastCollected"),
                       TRI_V8UInt64String<TRI_voc_tick_t>(isolate, static_cast<TRI_voc_tick_t>(value)));
    }
  }

  // lastSealedId
  {
    auto value = std::get<2>(info);
    if (value == UINT64_MAX) {
      result->ForceSet(TRI_V8_ASCII_STRING("minLastSealed"), v8::Null(isolate));
    } else {
      result->ForceSet(TRI_V8_ASCII_STRING("minLastSealed"),
                       TRI_V8UInt64String<TRI_voc_tick_t>(isolate, static_cast<TRI_voc_tick_t>(value)));
    }
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

void MMFilesV8Functions::registerResources() {
  ISOLATE;
  v8::HandleScope scope(isolate);

  TRI_GET_GLOBALS();

  // patch ArangoCollection object
  v8::Handle<v8::ObjectTemplate> rt = v8::Handle<v8::ObjectTemplate>::New(isolate, v8g->VocbaseColTempl);
  TRI_ASSERT(!rt.IsEmpty());
  
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("datafiles"),
                       JS_DatafilesVocbaseCol, true);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("datafileScan"),
                       JS_DatafileScanVocbaseCol, true);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("rotate"),
                       JS_RotateVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("truncateDatafile"),
                       JS_TruncateDatafileVocbaseCol, true);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("tryRepairDatafile"),
                       JS_TryRepairDatafileVocbaseCol, true);
  
  // add global WAL handling functions
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING("WAL_FLUSH"), JS_FlushWal, true);
  TRI_AddGlobalFunctionVocbase(isolate, 
                               TRI_V8_ASCII_STRING("WAL_WAITCOLLECTOR"),
                               JS_WaitCollectorWal, true);
  TRI_AddGlobalFunctionVocbase(isolate, 
                               TRI_V8_ASCII_STRING("WAL_PROPERTIES"),
                               JS_PropertiesWal, true);
  TRI_AddGlobalFunctionVocbase(isolate, 
                               TRI_V8_ASCII_STRING("WAL_TRANSACTIONS"),
                               JS_TransactionsWal, true);
}
