#include "public/fpdf_atomic.h"

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// Helper to cast opaque FPDF_OBJECT back to internal CPDF_Object
CPDF_Object* CPDFObjectFromFPDFObject(FPDF_OBJECT object) {
  return static_cast<CPDF_Object*>(object);
}

// Helper to return opaque FPDF_OBJECT
FPDF_OBJECT FPDFObjectFromCPDFObject(CPDF_Object* object) {
  return static_cast<FPDF_OBJECT>(object);
}

// Helper to calculate FPDF_WIDESTRING length (double-byte null terminated)
size_t GetWideStringLength(FPDF_WIDESTRING value) {
  if (!value) return 0;
  const unsigned short* ptr = reinterpret_cast<const unsigned short*>(value);
  size_t len = 0;
  while (*ptr) {
    len++;
    ptr++;
  }
  return len;
}


// ----------------------------------------------------------------------------
// 1. Document Level Access
// ----------------------------------------------------------------------------

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_GetRoot(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) return nullptr;
  return FPDFObjectFromCPDFObject(pDoc->GetMutableRoot());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_GetTrailer(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) return nullptr;
  // Trailer is on the Parser and usually const-only access
  CPDF_Parser* parser = pDoc->GetParser();
  if (!parser) return nullptr;

  // We explicitly cast away const because FPDF_GetTrailer implies intent to modify
  // for advanced users (atomic API).
  const CPDF_Dictionary* pTrailer = parser->GetTrailer();
  return FPDFObjectFromCPDFObject(const_cast<CPDF_Dictionary*>(pTrailer));
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_GetInfoDict(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) return nullptr;
  // GetInfo returns RetainPtr<CPDF_Dictionary> which is mutable
  return FPDFObjectFromCPDFObject(pDoc->GetInfo());
}


// ----------------------------------------------------------------------------
// 2. Object Creation & Indirect Objects
// ----------------------------------------------------------------------------

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewIndirectDict(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) return nullptr;
  auto pDict = pDoc->NewIndirect<CPDF_Dictionary>();
  return FPDFObjectFromCPDFObject(pDict.Get());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewIndirectArray(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) return nullptr;
  auto pArray = pDoc->NewIndirect<CPDF_Array>();
  return FPDFObjectFromCPDFObject(pArray.Get());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewDict(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) return nullptr;
  auto pDict = pdfium::MakeRetain<CPDF_Dictionary>(pDoc->GetByteStringPool());
  return FPDFObjectFromCPDFObject(pDict.Leak());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewArray(FPDF_DOCUMENT document) {
  auto pArray = pdfium::MakeRetain<CPDF_Array>();
  return FPDFObjectFromCPDFObject(pArray.Leak());
}


// ----------------------------------------------------------------------------
// 3. Scalar Creation
// ----------------------------------------------------------------------------

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewString(FPDF_DOCUMENT document, FPDF_WIDESTRING value) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);

  // Convert FPDF_WIDESTRING (unsigned short*) to WideString
  size_t len = GetWideStringLength(value);
  WideString ws = WideString::FromUTF16LE(
      pdfium::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value), len * sizeof(uint16_t)));

  // Construct using AsStringView()
  auto pStr = pdfium::MakeRetain<CPDF_String>(
      pDoc ? pDoc->GetByteStringPool() : nullptr,
      ws.AsStringView());

  return FPDFObjectFromCPDFObject(pStr.Leak());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewName(FPDF_DOCUMENT document, FPDF_BYTESTRING value) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  auto pName = pdfium::MakeRetain<CPDF_Name>(pDoc ? pDoc->GetByteStringPool() : nullptr, value);
  return FPDFObjectFromCPDFObject(pName.Leak());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewBoolean(FPDF_DOCUMENT document, FPDF_BOOL value) {
  auto pBool = pdfium::MakeRetain<CPDF_Boolean>(!!value);
  return FPDFObjectFromCPDFObject(pBool.Leak());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewNumber(FPDF_DOCUMENT document, float value) {
  auto pNum = pdfium::MakeRetain<CPDF_Number>(value);
  return FPDFObjectFromCPDFObject(pNum.Leak());
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewReference(FPDF_DOCUMENT document, FPDF_OBJECT object) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Object* pObj = CPDFObjectFromFPDFObject(object);

  // IsIndirect check substitute: !IsInline()
  if (!pDoc || !pObj || pObj->IsInline()) return nullptr;

  auto pRef = pdfium::MakeRetain<CPDF_Reference>(pDoc, pObj->GetObjNum());
  return FPDFObjectFromCPDFObject(pRef.Leak());
}


// ----------------------------------------------------------------------------
// 4. Dictionary Operations
// ----------------------------------------------------------------------------

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_Dict_Get(FPDF_OBJECT dict, FPDF_BYTESTRING key) {
  CPDF_Object* pObj = CPDFObjectFromFPDFObject(dict);
  if (!pObj || !pObj->IsDictionary()) return nullptr;

  // Use AsMutableDictionary
  return FPDFObjectFromCPDFObject(pObj->AsMutableDictionary()->GetMutableObjectFor(key));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Dict_Set(FPDF_OBJECT dict, FPDF_BYTESTRING key, FPDF_OBJECT value) {
  CPDF_Object* pDictObj = CPDFObjectFromFPDFObject(dict);
  CPDF_Object* pValObj = CPDFObjectFromFPDFObject(value);
  if (!pDictObj || !pDictObj->IsDictionary() || !key || !pValObj) return false;

  // Use AsMutableDictionary
  pDictObj->AsMutableDictionary()->SetFor(key, RetainPtr<CPDF_Object>(pValObj));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Dict_SetString(FPDF_OBJECT dict, FPDF_BYTESTRING key, FPDF_WIDESTRING value) {
  CPDF_Object* pDictObj = CPDFObjectFromFPDFObject(dict);
  if (!pDictObj || !pDictObj->IsDictionary() || !key) return false;

  size_t len = GetWideStringLength(value);
  WideString ws = WideString::FromUTF16LE(
      pdfium::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value), len * sizeof(uint16_t)));

  // Use AsMutableDictionary
  pDictObj->AsMutableDictionary()->SetNewFor<CPDF_String>(key, ws.AsStringView());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Dict_Remove(FPDF_OBJECT dict, FPDF_BYTESTRING key) {
  CPDF_Object* pObj = CPDFObjectFromFPDFObject(dict);
  if (!pObj || !pObj->IsDictionary() || !key) return false;

  // Use AsMutableDictionary
  pObj->AsMutableDictionary()->RemoveFor(key);
  return true;
}


// ----------------------------------------------------------------------------
// 5. Array Operations
// ----------------------------------------------------------------------------

FPDF_EXPORT int FPDF_CALLCONV FPDF_Array_Count(FPDF_OBJECT array) {
  CPDF_Object* pObj = CPDFObjectFromFPDFObject(array);
  if (!pObj || !pObj->IsArray()) return 0;
  // AsArray() is fine for reading size if we don't modify, but consistent formatting implies:
  return pObj->AsArray()->size();
}

FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_Array_Get(FPDF_OBJECT array, int index) {
  CPDF_Object* pObj = CPDFObjectFromFPDFObject(array);
  if (!pObj || !pObj->IsArray()) return nullptr;

  // AsMutableArray for potential modification of returned object
  return FPDFObjectFromCPDFObject(pObj->AsMutableArray()->GetMutableObjectAt(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Array_Add(FPDF_OBJECT array, FPDF_OBJECT value) {
  CPDF_Object* pArrayObj = CPDFObjectFromFPDFObject(array);
  CPDF_Object* pValObj = CPDFObjectFromFPDFObject(value);
  if (!pArrayObj || !pArrayObj->IsArray() || !pValObj) return false;

  // AsMutableArray
  pArrayObj->AsMutableArray()->Append(RetainPtr<CPDF_Object>(pValObj));
  return true;
}


// ----------------------------------------------------------------------------
// 6. Stream Operations
// ----------------------------------------------------------------------------

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Stream_GetData(FPDF_OBJECT stream, void* buffer, unsigned long buflen, unsigned long* out_buflen) {
  if (!out_buflen) return false;

  CPDF_Object* pObj = CPDFObjectFromFPDFObject(stream);
  if (!pObj) return false;

  // Resolve indirect references (e.g. /Metadata 10 0 R) to get the actual stream
  RetainPtr<const CPDF_Object> pDirect = pObj->GetDirect();
  if (!pDirect || !pDirect->IsStream()) return false;

  RetainPtr<const CPDF_Stream> pStream(pDirect->AsStream());

  // Use standard helper to decode and copy stream data
  *out_buflen = DecodeStreamMaybeCopyAndReturnLength(
      std::move(pStream),
      UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer),
                                  static_cast<size_t>(buflen))));

  return true;
}
