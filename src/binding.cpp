
#include "binding.hpp"

#include <sstream>
#include <cmath>
#include <cassert>
#include <cstring>

#include <protozero/pbf_writer.hpp>
#include <protozero/pbf_reader.hpp>

namespace carmen {

using namespace v8;

Nan::Persistent<FunctionTemplate> Cache::constructor;

inline std::vector<uint64_t> arrayToVector(Local<Array> const& array) {
    std::vector<uint64_t> cpp_array;
    cpp_array.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); i++) {
        int64_t js_value = array->Get(i)->IntegerValue();
        if (js_value < 0 || js_value >= std::numeric_limits<uint64_t>::max()) {
            std::stringstream s;
            s << "value in array too large (cannot fit '" << js_value << "' in uint64_t)";
            throw std::runtime_error(s.str());
        }
        cpp_array.emplace_back(static_cast<uint64_t>(js_value));
    }
    return cpp_array;
}

inline std::vector<std::string> arrayToStrVector(Local<Array> const& array) {
    std::vector<std::string> cpp_array;
    cpp_array.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); i++) {
        std::string js_value = *String::Utf8Value(array->Get(i)->ToString());;
        cpp_array.emplace_back(js_value);
    }
    return cpp_array;
}

inline Local<Array> vectorToArray(Cache::intarray const& vector) {
    std::size_t size = vector.size();
    Local<Array> array = Nan::New<Array>(static_cast<int>(size));
    for (uint32_t i = 0; i < size; i++) {
        array->Set(i, Nan::New<Number>(vector[i]));
    }
    return array;
}

inline Local<Object> mapToObject(std::map<std::uint64_t,std::uint64_t> const& map) {
    Local<Object> object = Nan::New<Object>();
    for (auto const& item : map) {
        object->Set(Nan::New<Number>(item.first), Nan::New<Number>(item.second));
    }
    return object;
}

inline rocksdb::DB* cacheGet(Cache const* c, std::string const& key) {
    Cache::message_cache const& messages = c->msg_;
    Cache::message_cache::const_iterator mitr = messages.find(key);
    return mitr->second->second;
}

inline bool cacheHas(Cache const* c, std::string const& key) {
    Cache::message_cache const& messages = c->msg_;
    Cache::message_cache::const_iterator mitr = messages.find(key);
    return mitr != messages.end();
}

inline void cacheInsert(Cache * c, std::string const& key, rocksdb::DB* const& message) {
    Cache::message_list &list = c->msglist_;
    Cache::message_cache &messages = c->msg_;
    Cache::message_cache::iterator mitr = messages.find(key);
    if (mitr == messages.end()) {
        list.emplace_front(std::make_pair(key, message));
        messages.emplace(key, list.begin());
        if (list.size() > c->cachesize) {
            messages.erase(list.back().first);
            list.pop_back();
        }
    }
}

inline bool cacheRemove(Cache * c, std::string const& key) {
    Cache::message_list &list = c->msglist_;
    Cache::message_cache &messages = c->msg_;
    Cache::message_cache::iterator mitr = messages.find(key);
    if (mitr != messages.end()) {
        list.erase(mitr->second);
        messages.erase(mitr);
        return true;
    } else {
        return false;
    }
}

Cache::intarray __get(Cache const* c, std::string const& type, std::string id, bool ignorePrefixFlag) {
    Cache::memcache const& mem = c->cache_;
    Cache::memcache::const_iterator itr = mem.find(type);
    Cache::intarray array;
    if (itr == mem.end()) {
        if (!cacheHas(c, type)) return array;

        rocksdb::DB* db = cacheGet(c, type);

        std::string search_id = id;
        for (uint32_t i = 0; i < 2; i++) {
            std::string message;
            rocksdb::Status s = db->Get(rocksdb::ReadOptions(), search_id, &message);
            if (s.ok()) {
                protozero::pbf_reader item(message);
                item.next(CACHE_ITEM);
                auto vals = item.get_packed_uint64();
                uint64_t lastval = 0;
                // delta decode values.
                for (auto it = vals.first; it != vals.second; ++it) {
                    if (lastval == 0) {
                        lastval = *it;
                        array.emplace_back(lastval);
                    } else {
                        lastval = lastval - *it;
                        array.emplace_back(lastval);
                    }
                }
            }
            if (i == 0) {
                if (!ignorePrefixFlag) {
                    break;
                } else {
                    search_id = search_id + ".";
                }
            }
        }
        return array;
    } else {
        Cache::arraycache::const_iterator aitr = itr->second.find(id);
        if (aitr == itr->second.end()) {
            if (ignorePrefixFlag) {
                Cache::arraycache::const_iterator aitr2 = itr->second.find(id + ".");
                if (aitr2 == itr->second.end()) {
                    return array;
                } else {
                    return aitr2->second;
                }
            }
            return array;
        } else {
            return aitr->second;
        }
    }
}

Cache::intarray __getbyprefix(Cache const* c, std::string const& type, std::string const& prefix) {
    Cache::intarray array;
    uint64_t prefix_length = prefix.length();
    const char* prefix_cstr = prefix.c_str();

    // Load values from memory cache
    Cache::memcache const& mem = c->cache_;
    Cache::memcache::const_iterator memshard = mem.find(type);
    if (memshard != mem.end()) {
        for (auto const& item : memshard->second) {
            const char* item_cstr = item.first.c_str();
            size_t item_length = item.first.length();
            // here, we skip this iteration if either the key is shorter than
            // the prefix, or, if the key has the no-prefix flag, its length
            // without the flag isn't the same as the item length (which will
            // make the prefix comparison in the next step effectively an
            // equality check)
            if (
                item_length < prefix_length ||
                (item_cstr[item_length - 1] == '.' && item_length != prefix_length + 1)
            ) {
                continue;
            }
            if (memcmp(prefix_cstr, item_cstr, prefix_length) == 0) {
                array.insert(array.end(), item.second.begin(), item.second.end());
            }
        }
    }

    // Load values from message cache
    if (cacheHas(c, type)) {
        rocksdb::DB* db = cacheGet(c, type);

        rocksdb::Iterator* rit = db->NewIterator(rocksdb::ReadOptions());
        for (rit->Seek(prefix); rit->Valid() && rit->key().ToString().compare(0, prefix.size(), prefix); rit->Next()) {
            std::string key_id = rit->key().ToString();

            // same skip operation as for the memory cache; see above
            if (
                key_id.length() < prefix.length() ||
                (key_id.at(key_id.length() - 1) == '.' && key_id.length() != prefix.length() + 1)
            ) {
                continue;
            }

            std::string message(rit->value().ToString());
            protozero::pbf_reader item(message);
            item.next(CACHE_ITEM);

            auto vals = item.get_packed_uint64();
            uint64_t lastval = 0;
            // delta decode values.
            for (auto it = vals.first; it != vals.second; ++it) {
                if (lastval == 0) {
                    lastval = *it;
                    array.emplace_back(lastval);
                } else {
                    lastval = lastval - *it;
                    array.emplace_back(lastval);
                }
            }
        }
    }

    return array;
}

void Cache::Initialize(Handle<Object> target) {
    Nan::HandleScope scope;
    Local<FunctionTemplate> t = Nan::New<FunctionTemplate>(Cache::New);
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(Nan::New("Cache").ToLocalChecked());
    Nan::SetPrototypeMethod(t, "has", has);
    Nan::SetPrototypeMethod(t, "loadSync", loadSync);
    Nan::SetPrototypeMethod(t, "pack", pack);
    Nan::SetPrototypeMethod(t, "merge", merge);
    Nan::SetPrototypeMethod(t, "list", list);
    Nan::SetPrototypeMethod(t, "_set", _set);
    Nan::SetPrototypeMethod(t, "_get", _get);
    Nan::SetPrototypeMethod(t, "unload", unload);
    Nan::SetMethod(t, "coalesce", coalesce);
    target->Set(Nan::New("Cache").ToLocalChecked(), t->GetFunction());
    constructor.Reset(t);
}

Cache::Cache()
  : ObjectWrap(),
    cache_(),
    msg_() {}

Cache::~Cache() { }

NAN_METHOD(Cache::pack)
{
    if (info.Length() < 2) {
        return Nan::ThrowTypeError("expected two info: 'filename', 'type'");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first argument must be a String");
    }
    if (!info[1]->IsString()) {
        return Nan::ThrowTypeError("second arg must be a String");
    }
    try {
        std::string filename = *String::Utf8Value(info[0]->ToString());
        std::string type = *String::Utf8Value(info[1]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::memcache const& mem = c->cache_;
        Cache::memcache::const_iterator itr = mem.find(type);
        if (itr != mem.end()) {
            rocksdb::DB* db;
            rocksdb::Options options;
            options.create_if_missing = true;
            rocksdb::Status status = rocksdb::DB::Open(options, filename, &db);

            if (!status.ok()) {
                return Nan::ThrowTypeError("unable to open rocksdb file for packing");
            }

            // Optimization idea: pre-pass on arrays to assemble guess about
            // how long the final message will be in order to be able to call
            // message.reserve(<length>)
            for (auto const& item : itr->second) {
                std::string message;
                message.clear();

                protozero::pbf_writer item_writer(message);
                std::size_t array_size = item.second.size();
                if (array_size > 0) {
                    // make copy of intarray so we can sort without
                    // modifying the original array
                    Cache::intarray varr = item.second;
                    // delta-encode values, sorted in descending order.
                    std::sort(varr.begin(), varr.end(), std::greater<uint64_t>());

                    {
                        // Using new (in protozero 1.3.0) packed writing API
                        // https://github.com/mapbox/protozero/commit/4e7e32ac5350ea6d3dcf78ff5e74faeee513a6e1
                        protozero::packed_field_uint64 field{item_writer, 1};
                        uint64_t lastval = 0;
                        for (auto const& vitem : varr) {
                            if (lastval == 0) {
                                field.add_element(static_cast<uint64_t>(vitem));
                            } else {
                                field.add_element(static_cast<uint64_t>(lastval - vitem));
                            }
                            lastval = vitem;
                        }
                    }

                    db->Put(rocksdb::WriteOptions(), item.first, message);
                }
            }
            delete db;
            info.GetReturnValue().Set(true);
            return;
        } else {
            if (!cacheHas(c, type)) {
                return Nan::ThrowTypeError("pack: cannot pack empty data");
            } else {
                info.GetReturnValue().Set(true);
                return;
            }
        }
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

struct MergeBaton : carmen::noncopyable {
    uv_work_t request;
    std::string filename1;
    std::string filename2;
    std::string filename3;
    std::string method;
    std::string error;
    Nan::Persistent<v8::Function> callback;
};

void mergeQueue(uv_work_t* req) {
    MergeBaton *baton = static_cast<MergeBaton *>(req->data);
    std::string const& filename1 = baton->filename1;
    std::string const& filename2 = baton->filename2;
    std::string const& filename3 = baton->filename3;
    std::string const& method = baton->method;

    // input 1
    rocksdb::DB* db1;
    rocksdb::Options options1;
    options1.create_if_missing = true;
    rocksdb::Status status1 = rocksdb::DB::Open(options1, filename1, &db1);
    if (!status1.ok()) {
        return Nan::ThrowTypeError("unable to open rocksdb input file #1");
    }

    // input 2
    rocksdb::DB* db2;
    rocksdb::Options options2;
    options2.create_if_missing = true;
    rocksdb::Status status2 = rocksdb::DB::Open(options2, filename2, &db2);
    if (!status2.ok()) {
        return Nan::ThrowTypeError("unable to open rocksdb input file #2");
    }

    // output
    rocksdb::DB* db3;
    rocksdb::Options options3;
    options3.create_if_missing = true;
    rocksdb::Status status3 = rocksdb::DB::Open(options3, filename3, &db3);
    if (!status1.ok()) {
        return Nan::ThrowTypeError("unable to open rocksdb output file");
    }

    // Ids that have been seen
    std::map<Cache::key_type,bool> ids1;
    std::map<Cache::key_type,bool> ids2;

    try {
        // Store ids from 1
        rocksdb::Iterator* it1 = db1->NewIterator(rocksdb::ReadOptions());
        for (it1->SeekToFirst(); it1->Valid(); it1->Next()) {
            ids1.emplace(it1->key().ToString(), true);
        }

        // Store ids from 2
        rocksdb::Iterator* it2 = db2->NewIterator(rocksdb::ReadOptions());
        for (it2->SeekToFirst(); it2->Valid(); it2->Next()) {
            ids2.emplace(it2->key().ToString(), true);
        }

        // No delta writes from message1
        it1 = db1->NewIterator(rocksdb::ReadOptions());
        for (it1->SeekToFirst(); it1->Valid(); it1->Next()) {
            std::string key_id = it1->key().ToString();

            // Skip this id if also in message 2
            if (ids2.find(key_id) != ids2.end()) continue;

            // get input proto
            std::string in_message = it1->value().ToString();
            protozero::pbf_reader item(in_message);
            item.next(CACHE_ITEM);

            std::string message;
            message.clear();

            protozero::pbf_writer item_writer(message);
            {
                protozero::packed_field_uint64 field{item_writer, 1};
                auto vals = item.get_packed_uint64();
                for (auto it = vals.first; it != vals.second; ++it) {
                    field.add_element(static_cast<uint64_t>(*it));
                }
            }

            rocksdb::Status putStatus = db3->Put(rocksdb::WriteOptions(), key_id, message);
            assert(putStatus.ok());
        }

        // No delta writes from message2
        it2 = db2->NewIterator(rocksdb::ReadOptions());
        for (it2->SeekToFirst(); it2->Valid(); it2->Next()) {
            std::string key_id = it2->key().ToString();


            // Skip this id if also in message 1
            if (ids1.find(key_id) != ids1.end()) continue;

            // get input proto
            std::string in_message = it2->value().ToString();
            protozero::pbf_reader item(in_message);
            item.next(CACHE_ITEM);

            std::string message;
            message.clear();

            protozero::pbf_writer item_writer(message);
            {
                protozero::packed_field_uint64 field{item_writer, 1};
                auto vals = item.get_packed_uint64();
                for (auto it = vals.first; it != vals.second; ++it) {
                    field.add_element(static_cast<uint64_t>(*it));
                }
            }

            rocksdb::Status putStatus = db3->Put(rocksdb::WriteOptions(), key_id, message);
            assert(putStatus.ok());
        }

        // Delta writes for ids in both message1 and message2
        it1 = db1->NewIterator(rocksdb::ReadOptions());
        for (it1->SeekToFirst(); it1->Valid(); it1->Next()) {
            std::string key_id = it1->key().ToString();

            // Skip ids that are only in one or the other lists
            if (ids1.find(key_id) == ids1.end() || ids2.find(key_id) == ids2.end()) continue;

            // get input proto
            std::string in_message1 = it1->value().ToString();
            protozero::pbf_reader item(in_message1);
            item.next(CACHE_ITEM);

            uint64_t lastval = 0;
            Cache::intarray varr;

            // Add values from filename1
            auto vals = item.get_packed_uint64();
            for (auto it = vals.first; it != vals.second; ++it) {
                if (method == "freq") {
                    varr.emplace_back(*it);
                    break;
                } else if (lastval == 0) {
                    lastval = *it;
                    varr.emplace_back(lastval);
                } else {
                    lastval = lastval - *it;
                    varr.emplace_back(lastval);
                }
            }

            std::string in_message2;
            rocksdb::Status s = db2->Get(rocksdb::ReadOptions(), key_id, &in_message2);
            if (s.ok()) {
                // get input proto 2
                protozero::pbf_reader item2(in_message2);
                item2.next(CACHE_ITEM);

                auto vals2 = item2.get_packed_uint64();
                lastval = 0;
                for (auto it = vals2.first; it != vals2.second; ++it) {
                    if (method == "freq") {
                        if (key_id == "__MAX__") {
                            varr[0] = varr[0] > *it ? varr[0] : *it;
                        } else {
                            varr[0] = varr[0] + *it;
                        }
                        break;
                    } else if (lastval == 0) {
                        lastval = *it;
                        varr.emplace_back(lastval);
                    } else {
                        lastval = lastval - *it;
                        varr.emplace_back(lastval);
                    }
                }
            }

            // Sort for proper delta encoding
            std::sort(varr.begin(), varr.end(), std::greater<uint64_t>());

            // Write varr to merged protobuf
            std::string message;
            message.clear();

            protozero::pbf_writer item_writer(message);
            {
                protozero::packed_field_uint64 field{item_writer, 1};
                lastval = 0;
                for (auto const& vitem : varr) {
                    if (lastval == 0) {
                        field.add_element(static_cast<uint64_t>(vitem));
                    } else {
                        field.add_element(static_cast<uint64_t>(lastval - vitem));
                    }
                    lastval = vitem;
                }
            }

            rocksdb::Status putStatus = db3->Put(rocksdb::WriteOptions(), key_id, message);
            assert(putStatus.ok());
        }

        delete db3;
    } catch (std::exception const& ex) {
        baton->error = ex.what();
    }
}

void mergeAfter(uv_work_t* req) {
    Nan::HandleScope scope;
    MergeBaton *baton = static_cast<MergeBaton *>(req->data);
    if (!baton->error.empty()) {
        v8::Local<v8::Value> argv[1] = { Nan::Error(baton->error.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(baton->callback), 1, argv);
    } else {
        Local<Value> argv[2] = { Nan::Null() };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(baton->callback), 1, argv);
    }
    baton->callback.Reset();
    delete baton;
}

NAN_METHOD(Cache::list)
{
    if (info.Length() < 1) {
        return Nan::ThrowTypeError("expected one arg: 'type'");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first argument must be a String");
    }
    try {
        std::string type = *String::Utf8Value(info[0]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::memcache const& mem = c->cache_;
        Cache::message_cache const& messages = c->msg_;
        Local<Array> ids = Nan::New<Array>();

        Cache::memcache::const_iterator itr = mem.find(type);
        unsigned idx = 0;
        if (itr != mem.end()) {
            for (auto const& item : itr->second) {
                ids->Set(idx++,Nan::New(item.first).ToLocalChecked());
            }
        }

        // parse message for ids
        if (cacheHas(c, type)) {
            rocksdb::DB* db = cacheGet(c, type);

            rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                std::string key_id = it->key().ToString();
                ids->Set(idx++, Nan::New(key_id).ToLocalChecked());
            }
        }

        info.GetReturnValue().Set(ids);
        return;
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

NAN_METHOD(Cache::merge)
{
    if (!info[0]->IsString()) return Nan::ThrowTypeError("argument 1 must be a String (infile 1)");
    if (!info[1]->IsString()) return Nan::ThrowTypeError("argument 2 must be a String (infile 2)");
    if (!info[2]->IsString()) return Nan::ThrowTypeError("argument 3 must be a String (outfile)");
    if (!info[3]->IsString()) return Nan::ThrowTypeError("argument 4 must be a String (method)");
    if (!info[4]->IsFunction()) return Nan::ThrowTypeError("argument 5 must be a callback function");

    std::string in1 = *String::Utf8Value(info[0]->ToString());
    std::string in2 = *String::Utf8Value(info[1]->ToString());
    std::string out = *String::Utf8Value(info[2]->ToString());
    Local<Value> callback = info[4];
    std::string method = *String::Utf8Value(info[3]->ToString());

    MergeBaton *baton = new MergeBaton();
    baton->filename1 = in1;
    baton->filename2 = in2;
    baton->filename3 = out;
    baton->method = method;
    baton->callback.Reset(callback.As<Function>());
    baton->request.data = baton;
    uv_queue_work(uv_default_loop(), &baton->request, mergeQueue, (uv_after_work_cb)mergeAfter);

    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

NAN_METHOD(Cache::_set)
{
    if (info.Length() < 3) {
        return Nan::ThrowTypeError("expected three info: 'type', 'id', 'data'");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first argument must be a String");
    }
    if (!info[1]->IsString()) {
        return Nan::ThrowTypeError("second arg must be a String");
    }
    if (!info[2]->IsArray()) {
        return Nan::ThrowTypeError("third arg must be an Array");
    }
    Local<Array> data = Local<Array>::Cast(info[2]);
    if (data->IsNull() || data->IsUndefined()) {
        return Nan::ThrowTypeError("an array expected for third argument");
    }
    try {
        std::string type = *String::Utf8Value(info[0]->ToString());
        std::string id = *String::Utf8Value(info[1]->ToString());

        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::memcache & mem = c->cache_;
        Cache::memcache::const_iterator itr = mem.find(type);
        if (itr == mem.end()) {
            c->cache_.emplace(type, Cache::arraycache());
        }
        Cache::arraycache & arrc = c->cache_[type];
        Cache::arraycache::key_type key_id = static_cast<Cache::arraycache::key_type>(id);
        Cache::arraycache::iterator itr2 = arrc.find(key_id);
        if (itr2 == arrc.end()) {
            arrc.emplace(key_id,Cache::intarray());
        }
        Cache::intarray & vv = arrc[key_id];

        unsigned array_size = data->Length();
        if (info[3]->IsBoolean() && info[3]->BooleanValue()) {
            vv.reserve(vv.size() + array_size);
        } else {
            if (itr2 != arrc.end()) vv.clear();
            vv.reserve(array_size);
        }

        for (unsigned i=0;i<array_size;++i) {
            vv.emplace_back(static_cast<uint64_t>(data->Get(i)->NumberValue()));
        }
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

NAN_METHOD(Cache::loadSync)
{
    if (info.Length() < 2) {
        return Nan::ThrowTypeError("expected two info: 'filename', 'type'");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first arg 'filename' must be a String");
    }
    if (!info[1]->IsString()) {
        return Nan::ThrowTypeError("second arg 'type' must be a String");
    }
    try {
        std::string filename = *String::Utf8Value(info[0]->ToString());
        std::string type = *String::Utf8Value(info[1]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::memcache & mem = c->cache_;
        Cache::memcache::iterator itr = mem.find(type);
        if (itr != mem.end()) {
            c->cache_.emplace(type, arraycache());
        }
        Cache::memcache::iterator itr2 = mem.find(type);
        if (itr2 != mem.end()) {
            mem.erase(itr2);
        }
        if (!cacheHas(c, type)) {
            rocksdb::DB* db;
            rocksdb::Options options;
            options.create_if_missing = true;
            rocksdb::Status status = rocksdb::DB::Open(options, filename, &db);

            if (!status.ok()) {
                return Nan::ThrowTypeError("unable to open rocksdb file for loading");
            }

            cacheInsert(c, type, db);
        }
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

NAN_METHOD(Cache::has)
{
    if (info.Length() < 1) {
        return Nan::ThrowTypeError("expected one info: 'type'");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first arg must be a String");
    }
    try {
        std::string type = *String::Utf8Value(info[0]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::memcache const& mem = c->cache_;
        Cache::memcache::const_iterator itr = mem.find(type);
        if (itr != mem.end()) {
            info.GetReturnValue().Set(Nan::True());
            return;
        } else {
            if (cacheHas(c, type)) {
                info.GetReturnValue().Set(Nan::True());
                return;
            } else {
                info.GetReturnValue().Set(Nan::False());
                return;
            }
        }
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::_get)
{
    if (info.Length() < 2) {
        return Nan::ThrowTypeError("expected two info: type and id");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first arg must be a String");
    }
    if (!info[1]->IsString()) {
        return Nan::ThrowTypeError("second arg must be a String");
    }
    try {
        std::string type = *String::Utf8Value(info[0]->ToString());
        std::string id = *String::Utf8Value(info[1]->ToString());

        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::intarray vector = __get(c, type, id, false);
        if (!vector.empty()) {
            info.GetReturnValue().Set(vectorToArray(vector));
            return;
        } else {
            info.GetReturnValue().Set(Nan::Undefined());
            return;
        }
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::unload)
{
    if (info.Length() < 1) {
        return Nan::ThrowTypeError("expected one info: 'type'");
    }
    if (!info[0]->IsString()) {
        return Nan::ThrowTypeError("first arg must be a String");
    }
    bool hit = false;
    try {
        std::string type = *String::Utf8Value(info[0]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(info.This());
        Cache::memcache & mem = c->cache_;
        Cache::memcache::iterator itr = mem.find(type);
        if (itr != mem.end()) {
            hit = true;
            mem.erase(itr);
        }
        hit = hit || cacheRemove(c, type);
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
    info.GetReturnValue().Set(Nan::New<Boolean>(hit));
    return;
}

NAN_METHOD(Cache::New)
{
    if (!info.IsConstructCall()) {
        return Nan::ThrowTypeError("Cannot call constructor as function, you need to use 'new' keyword");
    }
    try {
        if (info.Length() < 1) {
            return Nan::ThrowTypeError("expected 'id' argument");
        }
        if (!info[0]->IsString()) {
            return Nan::ThrowTypeError("first argument 'id' must be a String");
        }
        Cache* im = new Cache();

        if (info[1]->IsNumber()) {
            im->cachesize = static_cast<unsigned>(info[1]->NumberValue());
        }

        im->Wrap(info.This());
        info.This()->Set(Nan::New("id").ToLocalChecked(), info[0]);
        info.GetReturnValue().Set(info.This());
        return;
    } catch (std::exception const& ex) {
        return Nan::ThrowTypeError(ex.what());
    }
    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

//relev = 5 bits
//count = 3 bits
//reason = 12 bits
//* 1 bit gap
//id = 32 bits
constexpr double _pow(double x, int y)
{
    return y == 0 ? 1.0 : x * _pow(x, y-1);
}

constexpr uint64_t POW2_51 = static_cast<uint64_t>(_pow(2.0,51));
constexpr uint64_t POW2_48 = static_cast<uint64_t>(_pow(2.0,48));
constexpr uint64_t POW2_34 = static_cast<uint64_t>(_pow(2.0,34));
constexpr uint64_t POW2_28 = static_cast<uint64_t>(_pow(2.0,28));
constexpr uint64_t POW2_25 = static_cast<uint64_t>(_pow(2.0,25));
constexpr uint64_t POW2_20 = static_cast<uint64_t>(_pow(2.0,20));
constexpr uint64_t POW2_14 = static_cast<uint64_t>(_pow(2.0,14));
constexpr uint64_t POW2_3 = static_cast<uint64_t>(_pow(2.0,3));
constexpr uint64_t POW2_2 = static_cast<uint64_t>(_pow(2.0,2));

struct PhrasematchSubq {
    carmen::Cache *cache;
    double weight;
    std::string phrase;
    bool prefix;
    unsigned short idx;
    unsigned short zoom;
};

struct Cover {
    double relev;
    uint32_t id;
    uint32_t tmpid;
    unsigned short x;
    unsigned short y;
    unsigned short score;
    unsigned short idx;
    unsigned short subq;
    unsigned distance;
    double scoredist;
};

struct Context {
    std::vector<Cover> coverList;
    double relev;
};

Cover numToCover(uint64_t num) {
    Cover cover;
    assert(((num >> 34) % POW2_14) <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(((num >> 34) % POW2_14) >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short y = static_cast<unsigned short>((num >> 34) % POW2_14);
    assert(((num >> 20) % POW2_14) <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(((num >> 20) % POW2_14) >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short x = static_cast<unsigned short>((num >> 20) % POW2_14);
    assert(((num >> 48) % POW2_3) <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(((num >> 48) % POW2_3) >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short score = static_cast<unsigned short>((num >> 48) % POW2_3);
    uint32_t id = static_cast<uint32_t>(num % POW2_20);
    cover.x = x;
    cover.y = y;
    double relev = 0.4 + (0.2 * static_cast<double>((num >> 51) % POW2_2));
    cover.relev = relev;
    cover.score = score;
    cover.id = id;

    // These are not derived from decoding the input num but by
    // external values after initialization.
    cover.idx = 0;
    cover.subq = 0;
    cover.tmpid = 0;
    cover.distance = 0;

    return cover;
};

struct ZXY {
    unsigned z;
    unsigned x;
    unsigned y;
};

ZXY pxy2zxy(unsigned z, unsigned x, unsigned y, unsigned target_z) {
    ZXY zxy;
    zxy.z = target_z;

    // Interval between parent and target zoom level
    unsigned zDist = target_z - z;
    unsigned zMult = zDist - 1;
    if (zDist == 0) {
        zxy.x = x;
        zxy.y = y;
        return zxy;
    }

    // Midpoint length @ z for a tile at parent zoom level
    unsigned pMid_d = static_cast<unsigned>(std::pow(2,zDist) / 2);
    assert(pMid_d <= static_cast<double>(std::numeric_limits<unsigned>::max()));
    assert(pMid_d >= static_cast<double>(std::numeric_limits<unsigned>::min()));
    unsigned pMid = static_cast<unsigned>(pMid_d);
    zxy.x = (x * zMult) + pMid;
    zxy.y = (y * zMult) + pMid;
    return zxy;
}

ZXY bxy2zxy(unsigned z, unsigned x, unsigned y, unsigned target_z, bool max=false) {
    ZXY zxy;
    zxy.z = target_z;

    // Interval between parent and target zoom level
    signed zDist = target_z - z;
    if (zDist == 0) {
        zxy.x = x;
        zxy.y = y;
        return zxy;
    }

    // zoom conversion multiplier
    float mult = static_cast<float>(std::pow(2,zDist));

    // zoom in min
    if (zDist > 0 && !max) {
        zxy.x = static_cast<unsigned>(static_cast<float>(x) * mult);
        zxy.y = static_cast<unsigned>(static_cast<float>(y) * mult);
        return zxy;
    }
    // zoom in max
    else if (zDist > 0 && max) {
        zxy.x = static_cast<unsigned>(static_cast<float>(x) * mult + (mult - 1));
        zxy.y = static_cast<unsigned>(static_cast<float>(y) * mult + (mult - 1));
        return zxy;
    }
    // zoom out
    else {
        unsigned mod = static_cast<unsigned>(std::pow(2,target_z));
        unsigned xDiff = x % mod;
        unsigned yDiff = y % mod;
        unsigned newX = x - xDiff;
        unsigned newY = y - yDiff;

        zxy.x = static_cast<unsigned>(static_cast<float>(newX) * mult);
        zxy.y = static_cast<unsigned>(static_cast<float>(newY) * mult);
        return zxy;
    }
}

inline bool coverSortByRelev(Cover const& a, Cover const& b) noexcept {
    if (b.relev > a.relev) return false;
    else if (b.relev < a.relev) return true;
    else if (b.scoredist > a.scoredist) return false;
    else if (b.scoredist < a.scoredist) return true;
    else if (b.idx < a.idx) return false;
    else if (b.idx > a.idx) return true;
    else if (b.id < a.id) return false;
    else if (b.id > a.id) return true;
    // sorting by x and y is arbitrary but provides a more deterministic output order
    else if (b.x < a.x) return false;
    else if (b.x > a.x) return true;
    else return (b.y > a.y);
}

inline bool contextSortByRelev(Context const& a, Context const& b) noexcept {
    if (b.relev > a.relev) return false;
    else if (b.relev < a.relev) return true;
    else if (b.coverList[0].scoredist > a.coverList[0].scoredist) return false;
    else if (b.coverList[0].scoredist < a.coverList[0].scoredist) return true;
    else if (b.coverList[0].idx < a.coverList[0].idx) return false;
    else if (b.coverList[0].idx > a.coverList[0].idx) return true;
    return (b.coverList[0].id > a.coverList[0].id);
}

inline unsigned tileDist(unsigned ax, unsigned bx, unsigned ay, unsigned by) noexcept {
    return (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
}

struct CoalesceBaton : carmen::noncopyable {
    uv_work_t request;
    // params
    std::vector<PhrasematchSubq> stack;
    std::vector<uint64_t> centerzxy;
    std::vector<uint64_t> bboxzxy;
    Nan::Persistent<v8::Function> callback;
    // return
    std::vector<Context> features;
    // error
    std::string error;
};

// 32 tiles is about 40 miles at z14.
// Simulates 40 mile cutoff in carmen.
double scoredist(unsigned zoom, double distance, double score) {
    if (distance == 0.0) distance = 0.01;
    double scoredist = 0;
    if (zoom >= 14) scoredist = 32.0 / distance;
    if (zoom == 13) scoredist = 16.0 / distance;
    if (zoom == 12) scoredist = 8.0 / distance;
    if (zoom == 11) scoredist = 4.0 / distance;
    if (zoom == 10) scoredist = 2.0 / distance;
    if (zoom <= 9)  scoredist = 1.0 / distance;
    return score > scoredist ? score : scoredist;
}

void coalesceFinalize(CoalesceBaton* baton, std::vector<Context> const& contexts) {
    if (contexts.size() > 0) {
        // Coalesce stack, generate relevs.
        double relevMax = contexts[0].relev;
        unsigned short total = 0;
        std::map<uint64_t,bool> sets;
        std::map<uint64_t,bool>::iterator sit;

        for (unsigned short i = 0; i < contexts.size(); i++) {
            // Maximum allowance of coalesced features: 40.
            if (total >= 40) break;

            Context const& feature = contexts[i];

            // Since `coalesced` is sorted by relev desc at first
            // threshold miss we can break the loop.
            if (relevMax - feature.relev >= 0.25) break;

            // Only collect each feature once.
            sit = sets.find(feature.coverList[0].tmpid);
            if (sit != sets.end()) continue;

            sets.emplace(feature.coverList[0].tmpid, true);
            baton->features.emplace_back(feature);
            total++;
        }
    }
}
void coalesceSingle(uv_work_t* req) {
    CoalesceBaton *baton = static_cast<CoalesceBaton *>(req->data);

    try {
        std::vector<PhrasematchSubq> const& stack = baton->stack;
        PhrasematchSubq const& subq = stack[0];

        // proximity (optional)
        bool proximity = !baton->centerzxy.empty();
        unsigned cz;
        unsigned cx;
        unsigned cy;
        if (proximity) {
            cz = static_cast<unsigned>(baton->centerzxy[0]);
            cx = static_cast<unsigned>(baton->centerzxy[1]);
            cy = static_cast<unsigned>(baton->centerzxy[2]);
        } else {
            cz = 0;
            cx = 0;
            cy = 0;
        }

        // bbox (optional)
        bool bbox = !baton->bboxzxy.empty();
        unsigned minx;
        unsigned miny;
        unsigned maxx;
        unsigned maxy;
        if (bbox) {
            minx = static_cast<unsigned>(baton->bboxzxy[1]);
            miny = static_cast<unsigned>(baton->bboxzxy[2]);
            maxx = static_cast<unsigned>(baton->bboxzxy[3]);
            maxy = static_cast<unsigned>(baton->bboxzxy[4]);
        } else {
            minx = 0;
            miny = 0;
            maxx = 0;
            maxy = 0;
        }

        // Load and concatenate grids for all ids in `phrases`
        std::string type = "grid";
        Cache::intarray grids;
        if (subq.prefix) {
            grids = __getbyprefix(subq.cache, type, subq.phrase);
        } else {
            grids = __get(subq.cache, type, subq.phrase, true);
        }

        unsigned long m = grids.size();
        double relevMax = 0;
        std::vector<Cover> covers;
        covers.reserve(m);

        for (unsigned long j = 0; j < m; j++) {
            Cover cover = numToCover(grids[j]);
            cover.idx = subq.idx;
            cover.tmpid = static_cast<uint32_t>(cover.idx * POW2_25 + cover.id);
            cover.relev = cover.relev * subq.weight;
            cover.distance = proximity ? tileDist(cx, cover.x, cy, cover.y) : 0;
            cover.scoredist = proximity ? scoredist(cz, cover.distance, cover.score) : cover.score;

            // short circuit based on relevMax thres
            if (relevMax - cover.relev >= 0.25) continue;
            if (cover.relev > relevMax) relevMax = cover.relev;

            if (bbox) {
                if (cover.x < minx || cover.y < miny || cover.x > maxx || cover.y > maxy) continue;
            }

            covers.emplace_back(cover);
        }

        // sort grids by distance to proximity point
        std::sort(covers.begin(), covers.end(), coverSortByRelev);

        uint32_t lastid = 0;
        unsigned short added = 0;
        std::vector<Context> contexts;
        m = covers.size();
        contexts.reserve(m);
        for (unsigned long j = 0; j < m; j++) {
            // Stop at 40 contexts
            if (added == 40) break;

            // Attempt not to add the same feature but by diff cover twice
            if (lastid == covers[j].id) continue;

            lastid = covers[j].id;
            added++;

            Context context;
            context.coverList.emplace_back(covers[j]);
            context.relev = covers[j].relev;
            contexts.emplace_back(context);
        }

        coalesceFinalize(baton, contexts);
    } catch (std::exception const& ex) {
        baton->error = ex.what();
    }
}

void coalesceMulti(uv_work_t* req) {
    CoalesceBaton *baton = static_cast<CoalesceBaton *>(req->data);

    try {
        std::vector<PhrasematchSubq> const& stack = baton->stack;

        size_t size;

        // Cache zoom levels to iterate over as coalesce occurs.
        Cache::intarray zoom;
        std::vector<bool> zoomUniq(22);
        std::vector<Cache::intarray> zoomCache(22);
        size = stack.size();
        for (unsigned short i = 0; i < size; i++) {
            if (zoomUniq[stack[i].zoom]) continue;
            zoomUniq[stack[i].zoom] = true;
            zoom.emplace_back(stack[i].zoom);
        }

        size = zoom.size();
        for (unsigned short i = 0; i < size; i++) {
            Cache::intarray sliced;
            sliced.reserve(i);
            for (unsigned short j = 0; j < i; j++) {
                sliced.emplace_back(zoom[j]);
            }
            std::reverse(sliced.begin(), sliced.end());
            zoomCache[zoom[i]] = sliced;
        }

        // Coalesce relevs into higher zooms, e.g.
        // z5 inherits relev of overlapping tiles at z4.
        // @TODO assumes sources are in zoom ascending order.
        std::string type = "grid";
        std::map<uint64_t,std::vector<Cover>> coalesced;
        std::map<uint64_t,std::vector<Cover>>::iterator cit;
        std::map<uint64_t,std::vector<Cover>>::iterator pit;
        std::map<uint64_t,bool> done;
        std::map<uint64_t,bool>::iterator dit;

        size = stack.size();

        // proximity (optional)
        bool proximity = baton->centerzxy.size() > 0;
        unsigned cz;
        unsigned cx;
        unsigned cy;
        if (proximity) {
            cz = static_cast<unsigned>(baton->centerzxy[0]);
            cx = static_cast<unsigned>(baton->centerzxy[1]);
            cy = static_cast<unsigned>(baton->centerzxy[2]);
        } else {
            cz = 0;
            cx = 0;
            cy = 0;
        }

        // bbox (optional)
        bool bbox = !baton->bboxzxy.empty();
        unsigned bboxz;
        unsigned minx;
        unsigned miny;
        unsigned maxx;
        unsigned maxy;
        if (bbox) {
            bboxz = static_cast<unsigned>(baton->bboxzxy[0]);
            minx = static_cast<unsigned>(baton->bboxzxy[1]);
            miny = static_cast<unsigned>(baton->bboxzxy[2]);
            maxx = static_cast<unsigned>(baton->bboxzxy[3]);
            maxy = static_cast<unsigned>(baton->bboxzxy[4]);
        } else {
            bboxz = 0;
            minx = 0;
            miny = 0;
            maxx = 0;
            maxy = 0;
        }

        for (unsigned short i = 0; i < size; i++) {
            PhrasematchSubq const& subq = stack[i];

            // Load and concatenate grids for all ids in `phrases`
            std::string type = "grid";
            Cache::intarray grids;
            if (subq.prefix) {
                grids = __getbyprefix(subq.cache, type, subq.phrase);
            } else {
                grids = __get(subq.cache, type, subq.phrase, true);
            }

            unsigned short z = subq.zoom;
            auto const& zCache = zoomCache[z];
            std::size_t zCacheSize = zCache.size();

            unsigned long m = grids.size();

            for (unsigned long j = 0; j < m; j++) {
                Cover cover = numToCover(grids[j]);
                cover.idx = subq.idx;
                cover.subq = i;
                cover.tmpid = static_cast<uint32_t>(cover.idx * POW2_25 + cover.id);
                cover.relev = cover.relev * subq.weight;
                if (proximity) {
                    ZXY dxy = pxy2zxy(z, cover.x, cover.y, cz);
                    cover.distance = tileDist(cx, dxy.x, cy, dxy.y);
                    cover.scoredist = scoredist(cz, cover.distance, cover.score);
                } else {
                    cover.distance = 0;
                    cover.scoredist = cover.score;
                }

                if (bbox) {
                    ZXY min = bxy2zxy(bboxz, minx, miny, z, false);
                    ZXY max = bxy2zxy(bboxz, maxx, maxy, z, true);
                    if (cover.x < min.x || cover.y < min.y || cover.x > max.x || cover.y > max.y) continue;
                }

                uint64_t zxy = (z * POW2_28) + (cover.x * POW2_14) + (cover.y);

                cit = coalesced.find(zxy);
                if (cit == coalesced.end()) {
                    std::vector<Cover> coverList;
                    coverList.push_back(cover);
                    coalesced.emplace(zxy, coverList);
                } else {
                    cit->second.push_back(cover);
                }

                dit = done.find(zxy);
                if (dit == done.end()) {
                    for (unsigned a = 0; a < zCacheSize; a++) {
                        uint64_t p = zCache[a];
                        double s = static_cast<double>(1 << (z-p));
                        uint64_t pxy = static_cast<uint64_t>(p * POW2_28) +
                            static_cast<uint64_t>(std::floor(cover.x/s) * POW2_14) +
                            static_cast<uint64_t>(std::floor(cover.y/s));
                        // Set a flag to ensure coalesce occurs only once per zxy.
                        pit = coalesced.find(pxy);
                        if (pit != coalesced.end()) {
                            cit = coalesced.find(zxy);
                            for (auto const& pArray : pit->second) {
                                cit->second.emplace_back(pArray);
                            }
                            done.emplace(zxy, true);
                            break;
                        }
                    }
                }
            }
        }

        std::vector<Context> contexts;
        for (auto const& matched : coalesced) {
            std::vector<Cover> const& coverList = matched.second;
            size_t coverSize = coverList.size();
            for (unsigned i = 0; i < coverSize; i++) {
                unsigned used = 1 << coverList[i].subq;
                unsigned lastMask = 0;
                double lastRelev = 0.0;
                double stacky = 0.0;

                unsigned coverPos = 0;

                Context context;
                context.coverList.emplace_back(coverList[i]);
                context.relev = coverList[i].relev;
                for (unsigned j = i+1; j < coverSize; j++) {
                    unsigned mask = 1 << coverList[j].subq;

                    // this cover is functionally identical with previous and
                    // is more relevant, replace the previous.
                    if (mask == lastMask && coverList[j].relev > lastRelev) {
                        context.relev -= lastRelev;
                        context.relev += coverList[j].relev;
                        context.coverList[coverPos] = coverList[j];

                        stacky = 1.0;
                        used = used | mask;
                        lastMask = mask;
                        lastRelev = coverList[j].relev;
                        coverPos++;
                    // this cover doesn't overlap with used mask.
                    } else if (!(used & mask)) {
                        context.coverList.emplace_back(coverList[j]);
                        context.relev += coverList[j].relev;

                        stacky = 1.0;
                        used = used | mask;
                        lastMask = mask;
                        lastRelev = coverList[j].relev;
                        coverPos++;
                    }
                    // all other cases conflict with existing mask. skip.
                }
                context.relev -= 0.01;
                context.relev += 0.01 * stacky;
                contexts.emplace_back(context);
            }
        }
        std::sort(contexts.begin(), contexts.end(), contextSortByRelev);
        coalesceFinalize(baton, contexts);
    } catch (std::exception const& ex) {
       baton->error = ex.what();
    }
}

Local<Object> coverToObject(Cover const& cover) {
    Local<Object> object = Nan::New<Object>();
    object->Set(Nan::New("x").ToLocalChecked(), Nan::New<Number>(cover.x));
    object->Set(Nan::New("y").ToLocalChecked(), Nan::New<Number>(cover.y));
    object->Set(Nan::New("relev").ToLocalChecked(), Nan::New<Number>(cover.relev));
    object->Set(Nan::New("score").ToLocalChecked(), Nan::New<Number>(cover.score));
    object->Set(Nan::New("id").ToLocalChecked(), Nan::New<Number>(cover.id));
    object->Set(Nan::New("idx").ToLocalChecked(), Nan::New<Number>(cover.idx));
    object->Set(Nan::New("tmpid").ToLocalChecked(), Nan::New<Number>(cover.tmpid));
    object->Set(Nan::New("distance").ToLocalChecked(), Nan::New<Number>(cover.distance));
    object->Set(Nan::New("scoredist").ToLocalChecked(), Nan::New<Number>(cover.scoredist));
    return object;
}
Local<Array> contextToArray(Context const& context) {
    std::size_t size = context.coverList.size();
    Local<Array> array = Nan::New<Array>(static_cast<int>(size));
    for (uint32_t i = 0; i < size; i++) {
        array->Set(i, coverToObject(context.coverList[i]));
    }
    array->Set(Nan::New("relev").ToLocalChecked(), Nan::New(context.relev));
    return array;
}
void coalesceAfter(uv_work_t* req) {
    Nan::HandleScope scope;
    CoalesceBaton *baton = static_cast<CoalesceBaton *>(req->data);

    if (!baton->error.empty()) {
        v8::Local<v8::Value> argv[1] = { Nan::Error(baton->error.c_str()) };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(baton->callback), 1, argv);
    }
    else {
        std::vector<Context> const& features = baton->features;

        Local<Array> jsFeatures = Nan::New<Array>(static_cast<int>(features.size()));
        for (unsigned short i = 0; i < features.size(); i++) {
            jsFeatures->Set(i, contextToArray(features[i]));
        }

        Local<Value> argv[2] = { Nan::Null(), jsFeatures };
        Nan::MakeCallback(Nan::GetCurrentContext()->Global(), Nan::New(baton->callback), 2, argv);
    }

    baton->callback.Reset();
    delete baton;
}
NAN_METHOD(Cache::coalesce) {
    // PhrasematchStack (js => cpp)
    if (!info[0]->IsArray()) {
        return Nan::ThrowTypeError("Arg 1 must be a PhrasematchSubq array");
    }
    CoalesceBaton *baton = new CoalesceBaton();

    try {
        std::vector<PhrasematchSubq> stack;
        const Local<Array> array = Local<Array>::Cast(info[0]);
        for (uint32_t i = 0; i < array->Length(); i++) {
            Local<Value> val = array->Get(i);
            if (!val->IsObject()) {
                delete baton;
                return Nan::ThrowTypeError("All items in array must be valid PhrasematchSubq objects");
            }
            Local<Object> jsStack = val->ToObject();
            if (jsStack->IsNull() || jsStack->IsUndefined()) {
                delete baton;
                return Nan::ThrowTypeError("All items in array must be valid PhrasematchSubq objects");
            }
            PhrasematchSubq subq;

            int64_t _idx = jsStack->Get(Nan::New("idx").ToLocalChecked())->IntegerValue();
            if (_idx < 0 || _idx > std::numeric_limits<unsigned short>::max()) {
                delete baton;
                return Nan::ThrowTypeError("encountered idx value too large to fit in unsigned short");
            }
            subq.idx = static_cast<unsigned short>(_idx);

            int64_t _zoom = jsStack->Get(Nan::New("zoom").ToLocalChecked())->IntegerValue();
            if (_zoom < 0 || _zoom > std::numeric_limits<unsigned short>::max()) {
                delete baton;
                return Nan::ThrowTypeError("encountered zoom value too large to fit in unsigned short");
            }
            subq.zoom = static_cast<unsigned short>(_zoom);

            subq.weight = jsStack->Get(Nan::New("weight").ToLocalChecked())->NumberValue();
            subq.phrase = *String::Utf8Value(jsStack->Get(Nan::New("phrase").ToLocalChecked())->ToString());

            subq.prefix = jsStack->Get(Nan::New("prefix").ToLocalChecked())->BooleanValue();

            // JS cache reference => cpp
            Local<Object> cache = Local<Object>::Cast(jsStack->Get(Nan::New("cache").ToLocalChecked()));
            subq.cache = node::ObjectWrap::Unwrap<Cache>(cache);

            stack.push_back(subq);
        }
        baton->stack = stack;

        // Options object (js => cpp)
        if (!info[1]->IsObject()) {
            delete baton;
            return Nan::ThrowTypeError("Arg 2 must be an options object");
        }
        const Local<Object> options = Local<Object>::Cast(info[1]);
        if (options->Has(Nan::New("centerzxy").ToLocalChecked())) {
            baton->centerzxy = arrayToVector(Local<Array>::Cast(options->Get(Nan::New("centerzxy").ToLocalChecked())));
        }

        if (options->Has(Nan::New("bboxzxy").ToLocalChecked())) {
            baton->bboxzxy = arrayToVector(Local<Array>::Cast(options->Get(Nan::New("bboxzxy").ToLocalChecked())));
        }

        // callback
        if (!info[2]->IsFunction()) {
            delete baton;
            return Nan::ThrowTypeError("Arg 3 must be a callback function");
        }
        Local<Value> callback = info[2];
        baton->callback.Reset(callback.As<Function>());

        // queue work
        baton->request.data = baton;
        // optimization: for stacks of 1, use coalesceSingle
        if (stack.size() == 1) {
            uv_queue_work(uv_default_loop(), &baton->request, coalesceSingle, (uv_after_work_cb)coalesceAfter);
        } else {
            uv_queue_work(uv_default_loop(), &baton->request, coalesceMulti, (uv_after_work_cb)coalesceAfter);
        }
    } catch (std::exception const& ex) {
        delete baton;
        return Nan::ThrowTypeError(ex.what());
    }

    info.GetReturnValue().Set(Nan::Undefined());
    return;
}

extern "C" {
    static void start(Handle<Object> target) {
        Cache::Initialize(target);
    }
}

} // namespace carmen


NODE_MODULE(carmen, carmen::start)
