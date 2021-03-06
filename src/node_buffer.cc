#include <node_buffer.h>

#include <assert.h>
#include <stdlib.h> // malloc, free
#include <v8.h>

#include <string.h> // memcpy

#include <arpa/inet.h>  // htons, htonl

#include <node.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

namespace node {

using namespace v8;

#define SLICE_ARGS(start_arg, end_arg)                               \
  if (!start_arg->IsInt32() || !end_arg->IsInt32()) {                \
    return ThrowException(Exception::TypeError(                      \
          String::New("Bad argument.")));                            \
  }                                                                  \
  int32_t start = start_arg->Int32Value();                           \
  int32_t end = end_arg->Int32Value();                               \
  if (start < 0 || end < 0) {                                        \
    return ThrowException(Exception::TypeError(                      \
          String::New("Bad argument.")));                            \
  }                                                                  \
  if (!(start <= end)) {                                             \
    return ThrowException(Exception::Error(                          \
          String::New("Must have start <= end")));                   \
  }                                                                  \
  if ((size_t)end > parent->length_) {                               \
    return ThrowException(Exception::Error(                          \
          String::New("end cannot be longer than parent.length")));  \
  }


static Persistent<String> length_symbol;
static Persistent<String> chars_written_sym;
static Persistent<String> write_sym;
Persistent<FunctionTemplate> Buffer::constructor_template;


static inline size_t base64_decoded_size(const char *src, size_t size) {
  const char *const end = src + size;
  const int remainder = size % 4;

  size = (size / 4) * 3;
  if (remainder) {
    if (size == 0 && remainder == 1) {
      // special case: 1-byte input cannot be decoded
      size = 0;
    } else {
      // non-padded input, add 1 or 2 extra bytes
      size += 1 + (remainder == 3);
    }
  }

  // check for trailing padding (1 or 2 bytes)
  if (size > 0) {
    if (end[-1] == '=') size--;
    if (end[-2] == '=') size--;
  }

  return size;
}


static size_t ByteLength (Handle<String> string, enum encoding enc) {
  HandleScope scope;

  if (enc == UTF8) {
    return string->Utf8Length();
  } else if (enc == BASE64) {
    String::Utf8Value v(string);
    return base64_decoded_size(*v, v.length());
  } else {
    return string->Length();
  }
}


Buffer* Buffer::New(size_t size) {
  HandleScope scope;

  Local<Value> arg = Integer::NewFromUnsigned(size);
  Local<Object> b = constructor_template->GetFunction()->NewInstance(1, &arg);

  return ObjectWrap::Unwrap<Buffer>(b);
}


char* Buffer::Data(Handle<Object> obj) {
  if (obj->HasIndexedPropertiesInPixelData()) {
    return (char*)obj->GetIndexedPropertiesPixelData();
  }

  HandleScope scope;

  // Return true for "SlowBuffer"
  if (constructor_template->HasInstance(obj)) {
    return ObjectWrap::Unwrap<Buffer>(obj)->data();
  }

  // Not a buffer.
  return NULL;
}


size_t Buffer::Length(Handle<Object> obj) {
  if (obj->HasIndexedPropertiesInPixelData()) {
    return (size_t)obj->GetIndexedPropertiesPixelDataLength();
  }

  HandleScope scope;

  // Return true for "SlowBuffer"
  if (constructor_template->HasInstance(obj)) {
    return ObjectWrap::Unwrap<Buffer>(obj)->length();
  }

  // Not a buffer.
  return 0;
}


Handle<Value> Buffer::New(const Arguments &args) {
  HandleScope scope;

  if (!args.IsConstructCall()) {
    Local<Value> argv[10];
    for (int i = 0; i < MIN(args.Length(), 10); i++) {
      argv[i] = args[i];
    }
    Local<Object> instance =
      constructor_template->GetFunction()->NewInstance(args.Length(), argv);
    return scope.Close(instance);
  }

  Buffer *buffer;
  if (args[0]->IsInt32()) {
    // var buffer = new Buffer(1024);
    size_t length = args[0]->Uint32Value();
    buffer = new Buffer(length);

  } else {
    return ThrowException(Exception::TypeError(String::New("Bad argument")));
  }

  buffer->Wrap(args.This());
  args.This()->SetIndexedPropertiesToExternalArrayData(buffer->data(),
                                                       kExternalUnsignedByteArray,
                                                       buffer->length());
  args.This()->Set(length_symbol, Integer::New(buffer->length_));

  return args.This();
}


Buffer::Buffer(size_t length) : ObjectWrap() {
  off_ = 0;
  length_ = length;
  data_ = new char[length_];

  V8::AdjustAmountOfExternalAllocatedMemory(sizeof(Buffer) + length_);
}


Buffer::~Buffer() {
  //fprintf(stderr, "free buffer (%d refs left)\n", blob_->refs);
  delete data_;
  V8::AdjustAmountOfExternalAllocatedMemory(-(sizeof(Buffer) + length_));
}


char* Buffer::data() {
  return data_;
}


Handle<Value> Buffer::BinarySlice(const Arguments &args) {
  HandleScope scope;
  Buffer *parent = ObjectWrap::Unwrap<Buffer>(args.This());
  SLICE_ARGS(args[0], args[1])

  char *data = parent->data() + start;
  //Local<String> string = String::New(data, end - start);

  Local<Value> b =  Encode(data, end - start, BINARY);

  return scope.Close(b);
}


Handle<Value> Buffer::AsciiSlice(const Arguments &args) {
  HandleScope scope;
  Buffer *parent = ObjectWrap::Unwrap<Buffer>(args.This());
  SLICE_ARGS(args[0], args[1])

  char* data = parent->data() + start;
  Local<String> string = String::New(data, end - start);

  return scope.Close(string);
}


Handle<Value> Buffer::Utf8Slice(const Arguments &args) {
  HandleScope scope;
  Buffer *parent = ObjectWrap::Unwrap<Buffer>(args.This());
  SLICE_ARGS(args[0], args[1])
  char *data = parent->data() + start;
  Local<String> string = String::New(data, end - start);
  return scope.Close(string);
}

static const char *base64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz"
                                  "0123456789+/";
static const int unbase64_table[] =
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63
  ,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1
  ,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14
  ,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1
  ,-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40
  ,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
  };


Handle<Value> Buffer::Base64Slice(const Arguments &args) {
  HandleScope scope;
  Buffer *parent = ObjectWrap::Unwrap<Buffer>(args.This());
  SLICE_ARGS(args[0], args[1])

  int n = end - start;
  int out_len = (n + 2 - ((n + 2) % 3)) / 3 * 4;
  char *out = new char[out_len];

  uint8_t bitbuf[3];
  int i = start; // data() index
  int j = 0; // out index
  char c;
  bool b1_oob, b2_oob;

  while (i < end) {
    bitbuf[0] = parent->data()[i++];

    if (i < end) {
      bitbuf[1] = parent->data()[i];
      b1_oob = false;
    }  else {
      bitbuf[1] = 0;
      b1_oob = true;
    }
    i++;

    if (i < end) {
      bitbuf[2] = parent->data()[i];
      b2_oob = false;
    }  else {
      bitbuf[2] = 0;
      b2_oob = true;
    }
    i++;


    c = bitbuf[0] >> 2;
    assert(c < 64);
    out[j++] = base64_table[c];
    assert(j < out_len);

    c = ((bitbuf[0] & 0x03) << 4) | (bitbuf[1] >> 4);
    assert(c < 64);
    out[j++] = base64_table[c];
    assert(j < out_len);

    if (b1_oob) {
      out[j++] = '=';
    } else {
      c = ((bitbuf[1] & 0x0F) << 2) | (bitbuf[2] >> 6);
      assert(c < 64);
      out[j++] = base64_table[c];
    }
    assert(j < out_len);

    if (b2_oob) {
      out[j++] = '=';
    } else {
      c = bitbuf[2] & 0x3F;
      assert(c < 64);
      out[j++]  = base64_table[c];
    }
    assert(j <= out_len);
  }

  Local<String> string = String::New(out, out_len);
  delete [] out;
  return scope.Close(string);
}


// var bytesCopied = buffer.copy(target, targetStart, sourceStart, sourceEnd);
Handle<Value> Buffer::Copy(const Arguments &args) {
  HandleScope scope;

  Buffer *source = ObjectWrap::Unwrap<Buffer>(args.This());

  if (!Buffer::HasInstance(args[0])) {
    return ThrowException(Exception::TypeError(String::New(
            "First arg should be a Buffer")));
  }

  Buffer *target = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());

  ssize_t target_start = args[1]->Int32Value();
  ssize_t source_start = args[2]->Int32Value();
  ssize_t source_end = args[3]->IsInt32() ? args[3]->Int32Value()
                                          : source->length();

  if (source_end < source_start) {
    return ThrowException(Exception::Error(String::New(
            "sourceEnd < sourceStart")));
  }

  // Copy 0 bytes; we're done
  if (source_end == source_start) {
    return scope.Close(Integer::New(0));
  }

  if (target_start < 0 || target_start >= target->length()) {
    return ThrowException(Exception::Error(String::New(
            "targetStart out of bounds")));
  }

  if (source_start < 0 || source_start >= source->length()) {
    return ThrowException(Exception::Error(String::New(
            "sourceStart out of bounds")));
  }

  if (source_end < 0 || source_end > source->length()) {
    return ThrowException(Exception::Error(String::New(
            "sourceEnd out of bounds")));
  }

  ssize_t to_copy = MIN(MIN(source_end - source_start,
                            target->length() - target_start), 
                            source->length() - source_start);
  

  // need to use slightly slower memmove is the ranges might overlap
  memmove((void*)(target->data() + target_start),
          (const void*)(source->data() + source_start),
          to_copy);

  return scope.Close(Integer::New(to_copy));
}


// var charsWritten = buffer.utf8Write(string, offset, [maxLength]);
Handle<Value> Buffer::Utf8Write(const Arguments &args) {
  HandleScope scope;
  Buffer *buffer = ObjectWrap::Unwrap<Buffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();

  size_t offset = args[1]->Uint32Value();

  if (s->Utf8Length() > 0 && offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  size_t max_length = args[2]->IsUndefined() ? buffer->length_ - offset
                                             : args[2]->Uint32Value();
  max_length = MIN(buffer->length_ - offset, max_length);

  char* p = buffer->data() + offset;

  int char_written;

  int written = s->WriteUtf8(p,
                             max_length,
                             &char_written,
                             String::HINT_MANY_WRITES_EXPECTED);

  constructor_template->GetFunction()->Set(chars_written_sym,
                                           Integer::New(char_written));

  if (written > 0 && p[written-1] == '\0') written--;

  return scope.Close(Integer::New(written));
}


// var charsWritten = buffer.asciiWrite(string, offset);
Handle<Value> Buffer::AsciiWrite(const Arguments &args) {
  HandleScope scope;

  Buffer *buffer = ObjectWrap::Unwrap<Buffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();

  size_t offset = args[1]->Int32Value();

  if (s->Length() > 0 && offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  size_t max_length = args[2]->IsUndefined() ? buffer->length_ - offset
                                             : args[2]->Uint32Value();
  max_length = MIN(s->Length(), MIN(buffer->length_ - offset, max_length));

  char *p = buffer->data() + offset;

  int written = s->WriteAscii(p,
                              0,
                              max_length,
                              String::HINT_MANY_WRITES_EXPECTED);
  return scope.Close(Integer::New(written));
}

// var bytesWritten = buffer.base64Write(string, offset, [maxLength]);
Handle<Value> Buffer::Base64Write(const Arguments &args) {
  HandleScope scope;

  assert(unbase64_table['/'] == 63);
  assert(unbase64_table['+'] == 62);
  assert(unbase64_table['T'] == 19);
  assert(unbase64_table['Z'] == 25);
  assert(unbase64_table['t'] == 45);
  assert(unbase64_table['z'] == 51);

  Buffer *buffer = ObjectWrap::Unwrap<Buffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  String::AsciiValue s(args[0]->ToString());
  size_t offset = args[1]->Int32Value();

  // handle zero-length buffers graciously
  if (offset == 0 && buffer->length_ == 0) {
    return scope.Close(Integer::New(0));
  }

  if (offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  const size_t size = base64_decoded_size(*s, s.length());
  if (size > buffer->length_ - offset) {
    // throw exception, don't silently truncate
    return ThrowException(Exception::TypeError(String::New(
            "Buffer too small")));
  }

  char a, b, c, d;
  char* dst = buffer->data() + offset;
  const char *src = *s;
  const char *const srcEnd = src + s.length();

  while (src < srcEnd) {
    const int remaining = srcEnd - src;
    if (remaining == 0 || *src == '=') break;
    a = unbase64_table[*src++];

    if (remaining == 1 || *src == '=') break;
    b = unbase64_table[*src++];
    *dst++ = (a << 2) | ((b & 0x30) >> 4);

    if (remaining == 2 || *src == '=') break;
    c = unbase64_table[*src++];
    *dst++ = ((b & 0x0F) << 4) | ((c & 0x3C) >> 2);

    if (remaining == 3 || *src == '=') break;
    d = unbase64_table[*src++];
    *dst++ = ((c & 0x03) << 6) | (d & 0x3F);
  }

  return scope.Close(Integer::New(size));
}


Handle<Value> Buffer::BinaryWrite(const Arguments &args) {
  HandleScope scope;

  Buffer *buffer = ObjectWrap::Unwrap<Buffer>(args.This());

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();

  size_t offset = args[1]->Int32Value();

  if (s->Length() > 0 && offset >= buffer->length_) {
    return ThrowException(Exception::TypeError(String::New(
            "Offset is out of bounds")));
  }

  char *p = (char*)buffer->data() + offset;

  size_t towrite = MIN((unsigned long) s->Length(), buffer->length_ - offset);

  int written = DecodeWrite(p, towrite, s, BINARY);
  return scope.Close(Integer::New(written));
}


// var nbytes = Buffer.byteLength("string", "utf8")
Handle<Value> Buffer::ByteLength(const Arguments &args) {
  HandleScope scope;

  if (!args[0]->IsString()) {
    return ThrowException(Exception::TypeError(String::New(
            "Argument must be a string")));
  }

  Local<String> s = args[0]->ToString();
  enum encoding e = ParseEncoding(args[1], UTF8);

  return scope.Close(Integer::New(node::ByteLength(s, e)));
}


Handle<Value> Buffer::MakeFastBuffer(const Arguments &args) {
  HandleScope scope;

  Buffer *buffer = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());
  Local<Object> fast_buffer = args[1]->ToObject();;
  uint32_t offset = args[2]->Uint32Value();
  uint32_t length = args[3]->Uint32Value();

  fast_buffer->SetIndexedPropertiesToPixelData((uint8_t*)buffer->data() + offset,
                                               length);

  return Undefined();
}


bool Buffer::HasInstance(v8::Handle<v8::Value> val) {
  if (!val->IsObject()) return false;
  v8::Local<v8::Object> obj = val->ToObject();

  if (obj->HasIndexedPropertiesInPixelData()) return true;

  // Return true for "SlowBuffer"
  if (constructor_template->HasInstance(obj)) return true;

  return false;
}


void Buffer::Initialize(Handle<Object> target) {
  HandleScope scope;

  length_symbol = Persistent<String>::New(String::NewSymbol("length"));
  chars_written_sym = Persistent<String>::New(String::NewSymbol("_charsWritten"));

  Local<FunctionTemplate> t = FunctionTemplate::New(Buffer::New);
  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
  constructor_template->SetClassName(String::NewSymbol("SlowBuffer"));

  // copy free
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "binarySlice", Buffer::BinarySlice);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "asciiSlice", Buffer::AsciiSlice);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "base64Slice", Buffer::Base64Slice);
  // TODO NODE_SET_PROTOTYPE_METHOD(t, "utf16Slice", Utf16Slice);
  // copy
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "utf8Slice", Buffer::Utf8Slice);

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "utf8Write", Buffer::Utf8Write);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "asciiWrite", Buffer::AsciiWrite);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "binaryWrite", Buffer::BinaryWrite);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "base64Write", Buffer::Base64Write);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "copy", Buffer::Copy);

  NODE_SET_METHOD(constructor_template->GetFunction(),
                  "byteLength",
                  Buffer::ByteLength);
  NODE_SET_METHOD(constructor_template->GetFunction(),
                  "makeFastBuffer",
                  Buffer::MakeFastBuffer);

  target->Set(String::NewSymbol("SlowBuffer"), constructor_template->GetFunction());
}


}  // namespace node

NODE_MODULE(node_buffer, node::Buffer::Initialize);
