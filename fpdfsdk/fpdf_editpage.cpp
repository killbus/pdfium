// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "public/fpdf_edit.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "constants/font_encodings.h"
#include "constants/page_object.h"
#include "core/fpdfapi/edit/cpdf_contentstream_write_utils.h"
#include "core/fpdfapi/edit/cpdf_page_resource_editor.h"
#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_annotlist.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_memcpy_wrappers.h"
#include "core/fxcrt/fx_string_wrappers.h"
#include "core/fxcrt/notreached.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_fontmapper.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_formfill.h"

#ifdef PDF_ENABLE_XFA
#include "fpdfsdk/fpdfxfa/cpdfxfa_context.h"
#include "fpdfsdk/fpdfxfa/cpdfxfa_page.h"
#endif  // PDF_ENABLE_XFA

namespace {

static_assert(FPDF_PAGEOBJ_TEXT ==
                  static_cast<int>(CPDF_PageObject::Type::kText),
              "FPDF_PAGEOBJ_TEXT/CPDF_PageObject::TEXT mismatch");
static_assert(FPDF_PAGEOBJ_PATH ==
                  static_cast<int>(CPDF_PageObject::Type::kPath),
              "FPDF_PAGEOBJ_PATH/CPDF_PageObject::PATH mismatch");
static_assert(FPDF_PAGEOBJ_IMAGE ==
                  static_cast<int>(CPDF_PageObject::Type::kImage),
              "FPDF_PAGEOBJ_IMAGE/CPDF_PageObject::IMAGE mismatch");
static_assert(FPDF_PAGEOBJ_SHADING ==
                  static_cast<int>(CPDF_PageObject::Type::kShading),
              "FPDF_PAGEOBJ_SHADING/CPDF_PageObject::SHADING mismatch");
static_assert(FPDF_PAGEOBJ_FORM ==
                  static_cast<int>(CPDF_PageObject::Type::kForm),
              "FPDF_PAGEOBJ_FORM/CPDF_PageObject::FORM mismatch");

FPDF_FONT LoadReusableTextStampEmbeddedFont(FPDF_DOCUMENT document,
                                            const uint8_t* font_data,
                                            uint32_t font_data_size) {
  if (!font_data || font_data_size == 0) {
    return nullptr;
  }
  return FPDFText_LoadFont(document, font_data, font_data_size,
                           FPDF_FONT_TRUETYPE, /*cid=*/true);
}

FPDF_FONT LoadReusableTextStampStandardFont(
    FPDF_DOCUMENT document,
    FPDF_BYTESTRING standard_font_name) {
  // Use the exact Base14 check, not GetStandardFontName(), which accepts
  // and normalizes aliases such as Arial and TimesNewRoman.
  if (!standard_font_name ||
      !CFX_FontMapper::IsStandardFontName(standard_font_name)) {
    return nullptr;
  }
  return FPDFText_LoadStandardFont(document, standard_font_name);
}

bool IsPageObject(CPDF_Page* pPage) {
  if (!pPage) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> pFormDict = pPage->GetDict();
  if (!pFormDict->KeyExist(pdfium::page_object::kType)) {
    return false;
  }

  RetainPtr<const CPDF_Name> pName =
      ToName(pFormDict->GetObjectFor(pdfium::page_object::kType)->GetDirect());
  return pName && pName->GetString() == "Page";
}

void CalcBoundingBox(CPDF_PageObject* pPageObj) {
  switch (pPageObj->GetType()) {
    case CPDF_PageObject::Type::kText: {
      break;
    }
    case CPDF_PageObject::Type::kPath: {
      CPDF_PathObject* pPathObj = pPageObj->AsPath();
      pPathObj->CalcBoundingBox();
      break;
    }
    case CPDF_PageObject::Type::kImage: {
      CPDF_ImageObject* pImageObj = pPageObj->AsImage();
      pImageObj->CalcBoundingBox();
      break;
    }
    case CPDF_PageObject::Type::kShading: {
      CPDF_ShadingObject* pShadingObj = pPageObj->AsShading();
      pShadingObj->CalcBoundingBox();
      break;
    }
    case CPDF_PageObject::Type::kForm: {
      CPDF_FormObject* pFormObj = pPageObj->AsForm();
      pFormObj->CalcBoundingBox();
      break;
    }
  }
}

RetainPtr<CPDF_Dictionary> GetMarkParamDict(FPDF_PAGEOBJECTMARK mark) {
  CPDF_ContentMarkItem* pMarkItem =
      CPDFContentMarkItemFromFPDFPageObjectMark(mark);
  return pMarkItem ? pMarkItem->GetParam() : nullptr;
}

RetainPtr<CPDF_Dictionary> GetOrCreateMarkParamsDict(FPDF_DOCUMENT document,
                                                     FPDF_PAGEOBJECTMARK mark) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return nullptr;
  }

  CPDF_ContentMarkItem* pMarkItem =
      CPDFContentMarkItemFromFPDFPageObjectMark(mark);
  if (!pMarkItem) {
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> pParams = pMarkItem->GetParam();
  if (!pParams) {
    pParams = pDoc->New<CPDF_Dictionary>();
    pMarkItem->SetDirectDict(pParams);
  }
  return pParams;
}

bool PageObjectContainsMark(CPDF_PageObject* pPageObj,
                            FPDF_PAGEOBJECTMARK mark) {
  const CPDF_ContentMarkItem* pMarkItem =
      CPDFContentMarkItemFromFPDFPageObjectMark(mark);
  return pMarkItem && pPageObj->GetContentMarks()->ContainsItem(pMarkItem);
}

CPDF_FormObject* CPDFFormObjectFromFPDFPageObject(FPDF_PAGEOBJECT page_object) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  return pPageObj ? pPageObj->AsForm() : nullptr;
}

const CPDF_PageObjectHolder* CPDFPageObjHolderFromFPDFFormObject(
    FPDF_PAGEOBJECT page_object) {
  CPDF_FormObject* pFormObject = CPDFFormObjectFromFPDFPageObject(page_object);
  return pFormObject ? pFormObject->form() : nullptr;
}

RetainPtr<CPDF_Stream> NewContentStream(CPDF_Document* doc,
                                        ByteStringView bytes) {
  return doc->NewIndirect<CPDF_Stream>(bytes.unsigned_span());
}

RetainPtr<CPDF_Stream> NewWrappedContentStream(CPDF_Document* doc,
                                               const uint8_t* data,
                                               unsigned long size) {
  fxcrt::ostringstream buf;
  buf << "q\n";
  if (size > 0) {
    // SAFETY: the public API contract requires `data` to point to `size` bytes.
    UNSAFE_BUFFERS(buf.write(reinterpret_cast<const char*>(data),
                             pdfium::checked_cast<std::streamsize>(size)));
  }
  buf << "\nQ\n";
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

RetainPtr<CPDF_Dictionary> NewExtGState(CPDF_Document* doc, float alpha) {
  auto ext_gstate = doc->NewIndirect<CPDF_Dictionary>();
  ext_gstate->SetNewFor<CPDF_Name>("Type", "ExtGState");
  ext_gstate->SetNewFor<CPDF_Number>("CA", alpha);
  ext_gstate->SetNewFor<CPDF_Number>("ca", alpha);
  return ext_gstate;
}

bool IsValidRawDeviceRgbSize(int image_width,
                             int image_height,
                             unsigned long rgb_size) {
  if (image_width <= 0 || image_height <= 0) {
    return false;
  }

  const uint64_t width = static_cast<uint64_t>(image_width);
  const uint64_t height = static_cast<uint64_t>(image_height);
  const uint64_t max_stream_length =
      static_cast<uint64_t>(std::numeric_limits<int>::max());
  if (width > max_stream_length / height) {
    return false;
  }

  const uint64_t pixels = width * height;
  if (pixels > max_stream_length / 3) {
    return false;
  }

  return pixels * 3 == static_cast<uint64_t>(rgb_size);
}

bool IsValidRawDeviceRgbaSize(int image_width,
                              int image_height,
                              unsigned long rgba_size) {
  if (image_width <= 0 || image_height <= 0) {
    return false;
  }

  const uint64_t width = static_cast<uint64_t>(image_width);
  const uint64_t height = static_cast<uint64_t>(image_height);
  const uint64_t max_stream_length =
      static_cast<uint64_t>(std::numeric_limits<int>::max());
  if (width > max_stream_length / height) {
    return false;
  }

  const uint64_t pixels = width * height;
  if (pixels > max_stream_length / 4) {
    return false;
  }

  return pixels * 4 == static_cast<uint64_t>(rgba_size);
}

RetainPtr<CPDF_Stream> NewRawDeviceRgbImageXObject(CPDF_Document* doc,
                                                   const uint8_t* rgb_data,
                                                   unsigned long rgb_size,
                                                   int image_width,
                                                   int image_height) {
  auto image_dict = doc->New<CPDF_Dictionary>();
  image_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  image_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
  image_dict->SetNewFor<CPDF_Number>("Width", image_width);
  image_dict->SetNewFor<CPDF_Number>("Height", image_height);
  image_dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
  image_dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);

  pdfium::span<const uint8_t> rgb_span =
      UNSAFE_BUFFERS(pdfium::span(rgb_data, rgb_size));
  DataVector<uint8_t> data(rgb_span.begin(), rgb_span.end());
  return doc->NewIndirect<CPDF_Stream>(std::move(data), std::move(image_dict));
}

RetainPtr<CPDF_Stream> NewRawDeviceRgbaImageXObject(CPDF_Document* doc,
                                                    const uint8_t* rgba_data,
                                                    unsigned long rgba_size,
                                                    int image_width,
                                                    int image_height) {
  const size_t pixels = static_cast<size_t>(image_width) *
                        static_cast<size_t>(image_height);
  DataVector<uint8_t> rgb_data;
  DataVector<uint8_t> alpha_data;
  rgb_data.reserve(pixels * 3);
  alpha_data.reserve(pixels);

  pdfium::span<const uint8_t> rgba_span =
      UNSAFE_BUFFERS(pdfium::span(rgba_data, rgba_size));
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    const size_t offset = pixel * 4;
    rgb_data.push_back(rgba_span[offset]);
    rgb_data.push_back(rgba_span[offset + 1]);
    rgb_data.push_back(rgba_span[offset + 2]);
    alpha_data.push_back(rgba_span[offset + 3]);
  }

  auto smask_dict = doc->New<CPDF_Dictionary>();
  smask_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  smask_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
  smask_dict->SetNewFor<CPDF_Number>("Width", image_width);
  smask_dict->SetNewFor<CPDF_Number>("Height", image_height);
  smask_dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceGray");
  smask_dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
  RetainPtr<CPDF_Stream> smask =
      doc->NewIndirect<CPDF_Stream>(std::move(alpha_data),
                                    std::move(smask_dict));

  auto image_dict = doc->New<CPDF_Dictionary>();
  image_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  image_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
  image_dict->SetNewFor<CPDF_Number>("Width", image_width);
  image_dict->SetNewFor<CPDF_Number>("Height", image_height);
  image_dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
  image_dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
  image_dict->SetFor("SMask",
                     pdfium::MakeRetain<CPDF_Reference>(doc,
                                                        smask->GetObjNum()));
  return doc->NewIndirect<CPDF_Stream>(std::move(rgb_data),
                                       std::move(image_dict));
}

bool IsReusableImageXObject(CPDF_Document* doc, uint32_t image_object_number) {
  if (!doc || image_object_number == 0) {
    return false;
  }

  RetainPtr<CPDF_Stream> image_stream =
      ToStream(doc->GetOrParseIndirectObject(image_object_number));
  if (!image_stream) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> image_dict = image_stream->GetDict();
  return image_dict && image_dict->GetNameFor("Type") == "XObject" &&
         image_dict->GetNameFor("Subtype") == "Image";
}

RetainPtr<CPDF_Dictionary> NewStandardHelveticaFont(CPDF_Document* doc) {
  auto font = doc->NewIndirect<CPDF_Dictionary>();
  font->SetNewFor<CPDF_Name>("Type", "Font");
  font->SetNewFor<CPDF_Name>("Subtype", "Type1");
  font->SetNewFor<CPDF_Name>("BaseFont", CFX_Font::kDefaultAnsiFontName);
  font->SetNewFor<CPDF_Name>("Encoding",
                             pdfium::font_encodings::kWinAnsiEncoding);
  return font;
}

bool EscapeAsciiLiteralString(const char* text,
                              unsigned long text_size,
                              ByteString* escaped) {
  if (!text || text_size == 0 || !escaped) {
    return false;
  }

  for (unsigned long i = 0; i < text_size; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch < 0x20 || ch > 0x7e) {
      return false;
    }
    if (ch == '\\' || ch == '(' || ch == ')') {
      *escaped += '\\';
    }
    *escaped += static_cast<char>(ch);
  }
  return true;
}

RetainPtr<CPDF_Stream> NewWrappedExtGStateContentStream(
    CPDF_Document* doc,
    ByteStringView resource_name,
    const uint8_t* data,
    unsigned long size) {
  fxcrt::ostringstream buf;
  buf << "q\n/";
  buf << ByteString(resource_name);
  buf << " gs\n";
  if (size > 0) {
    // SAFETY: the public API contract requires `data` to point to `size` bytes.
    UNSAFE_BUFFERS(buf.write(reinterpret_cast<const char*>(data),
                             pdfium::checked_cast<std::streamsize>(size)));
  }
  buf << "\nQ\n";
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

RetainPtr<CPDF_Stream> NewWrappedTextProbeContentStream(
    CPDF_Document* doc,
    ByteStringView gs_name,
    ByteStringView font_name,
    ByteStringView escaped_text,
    float x,
    float y,
    float font_size) {
  fxcrt::ostringstream buf;
  buf << "q\n/";
  buf << ByteString(gs_name);
  buf << " gs\nBT\n/";
  buf << ByteString(font_name);
  buf << " ";
  WriteFloat(buf, font_size) << " Tf\n1 0 0 1 ";
  WriteFloat(buf, x) << " ";
  WriteFloat(buf, y) << " Tm\n1 0 0 rg\n(";
  buf << ByteString(escaped_text);
  buf << ") Tj\nET\nQ\n";
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

RetainPtr<CPDF_Stream> NewWrappedUnicodeTextProbeContentStream(
    CPDF_Document* doc,
    ByteStringView gs_name,
    ByteStringView font_name,
    ByteStringView encoded_text,
    float x,
    float y,
    float font_size) {
  fxcrt::ostringstream buf;
  buf << "q\n/";
  buf << ByteString(gs_name);
  buf << " gs\nBT\n/";
  buf << ByteString(font_name);
  buf << " ";
  WriteFloat(buf, font_size) << " Tf\n1 0 0 1 ";
  WriteFloat(buf, x) << " ";
  WriteFloat(buf, y) << " Tm\n1 0 0 rg\n";
  buf << PDF_HexEncodeString(encoded_text);
  buf << " Tj\nET\nQ\n";
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

RetainPtr<CPDF_Stream> NewWrappedImageProbeContentStream(
    CPDF_Document* doc,
    ByteStringView gs_name,
    ByteStringView image_name,
    float x,
    float y,
    float draw_width,
    float draw_height) {
  fxcrt::ostringstream buf;
  buf << "q\n/";
  buf << ByteString(gs_name);
  buf << " gs\n";
  WriteFloat(buf, draw_width) << " 0 0 ";
  WriteFloat(buf, draw_height) << " ";
  WriteFloat(buf, x) << " ";
  WriteFloat(buf, y) << " cm\n/";
  buf << ByteString(image_name);
  buf << " Do\nQ\n";
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

bool IsValidPlacementMatrix(const CFX_Matrix& matrix) {
  return std::isfinite(matrix.a) && std::isfinite(matrix.b) &&
         std::isfinite(matrix.c) && std::isfinite(matrix.d) &&
         std::isfinite(matrix.e) && std::isfinite(matrix.f) &&
         matrix.a * matrix.d - matrix.b * matrix.c != 0.0f;
}

RetainPtr<CPDF_Stream> NewReusableTextStampPlacementContentStream(
    CPDF_Document* doc,
    ByteStringView xobject_name,
    const std::vector<CFX_Matrix>& placements) {
  fxcrt::ostringstream buf;
  for (const CFX_Matrix& placement : placements) {
    buf << "q\n";
    WriteMatrix(buf, placement) << " cm\n/"
                                << ByteString(xobject_name) << " Do\nQ\n";
  }
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

RetainPtr<CPDF_Stream> NewReusableImageXObjectPlacementContentStream(
    CPDF_Document* doc,
    ByteStringView gs_name,
    ByteStringView image_name,
    const std::vector<CFX_Matrix>& placements) {
  fxcrt::ostringstream buf;
  for (const CFX_Matrix& placement : placements) {
    buf << "q\n/" << ByteString(gs_name) << " gs\n";
    WriteMatrix(buf, placement) << " cm\n/"
                                << ByteString(image_name) << " Do\nQ\n";
  }
  return doc->NewIndirect<CPDF_Stream>(&buf);
}

bool EncodeUnicodeTextWithFont(const WideString& wide_text,
                               CPDF_Font* font,
                               ByteString* encoded_text) {
  if (wide_text.IsEmpty() || !font || !encoded_text) {
    return false;
  }

  for (wchar_t wc : wide_text) {
    const uint32_t charcode = font->CharCodeFromUnicode(wc);
    if (charcode == 0) {
      return false;
    }
    font->AppendChar(encoded_text, charcode);
  }
  return !encoded_text->IsEmpty();
}

bool CollectContentArrayRefs(RetainPtr<CPDF_Array> array,
                             std::vector<uint32_t>* object_numbers) {
  for (size_t i = 0; i < array->size(); ++i) {
    RetainPtr<CPDF_Reference> reference =
        ToReference(array->GetMutableObjectAt(i));
    if (!reference) {
      return false;
    }
    RetainPtr<CPDF_Object> direct = reference->GetMutableDirect();
    if (!direct || !direct->IsStream()) {
      return false;
    }
    object_numbers->push_back(reference->GetRefObjNum());
  }
  return true;
}

bool CollectOriginalContentRefs(RetainPtr<CPDF_Object> contents,
                                std::vector<uint32_t>* object_numbers) {
  if (!contents) {
    return true;
  }

  if (RetainPtr<CPDF_Reference> reference = ToReference(contents)) {
    RetainPtr<CPDF_Object> direct = reference->GetMutableDirect();
    if (!direct) {
      return false;
    }
    if (direct->IsStream()) {
      object_numbers->push_back(reference->GetRefObjNum());
      return true;
    }
    if (RetainPtr<CPDF_Array> array = ToArray(direct)) {
      return CollectContentArrayRefs(array, object_numbers);
    }
    return false;
  }

  if (RetainPtr<CPDF_Array> array = ToArray(contents)) {
    return CollectContentArrayRefs(array, object_numbers);
  }

  return false;
}

template <typename AppendStreamFactory>
bool ReplacePageContentsWithIsolatedAppendStream(
    CPDF_Document* doc,
    RetainPtr<CPDF_Dictionary> page_dict,
    const std::vector<uint32_t>& old_content_object_numbers,
    AppendStreamFactory append_stream_factory) {
  auto contents_array = doc->NewIndirect<CPDF_Array>();
  RetainPtr<CPDF_Stream> save_stream =
      NewContentStream(doc, ByteStringView("q\n"));
  RetainPtr<CPDF_Stream> restore_stream =
      NewContentStream(doc, ByteStringView("Q\n"));
  RetainPtr<CPDF_Stream> append_stream = append_stream_factory();
  if (!append_stream) {
    return false;
  }

  contents_array->AppendNew<CPDF_Reference>(doc, save_stream->GetObjNum());
  for (uint32_t object_number : old_content_object_numbers) {
    contents_array->AppendNew<CPDF_Reference>(doc, object_number);
  }
  contents_array->AppendNew<CPDF_Reference>(doc, restore_stream->GetObjNum());
  contents_array->AppendNew<CPDF_Reference>(doc, append_stream->GetObjNum());
  page_dict->SetNewFor<CPDF_Reference>(pdfium::page_object::kContents, doc,
                                       contents_array->GetObjNum());
  return true;
}

}  // namespace

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV FPDF_CreateNewDocument() {
  auto pDoc =
      std::make_unique<CPDF_Document>(std::make_unique<CPDF_DocRenderData>(),
                                      std::make_unique<CPDF_DocPageData>());
  pDoc->CreateNewDoc();

  time_t currentTime;
  ByteString DateStr;
  if (IsPDFSandboxPolicyEnabled(FPDF_POLICY_MACHINETIME_ACCESS)) {
    if (FXSYS_time(&currentTime) != -1) {
      tm* pTM = FXSYS_localtime(&currentTime);
      if (pTM) {
        DateStr = ByteString::Format(
            "D:%04d%02d%02d%02d%02d%02d", pTM->tm_year + 1900, pTM->tm_mon + 1,
            pTM->tm_mday, pTM->tm_hour, pTM->tm_min, pTM->tm_sec);
      }
    }
  }

  RetainPtr<CPDF_Dictionary> pInfoDict = pDoc->GetInfo();
  if (pInfoDict) {
    if (IsPDFSandboxPolicyEnabled(FPDF_POLICY_MACHINETIME_ACCESS)) {
      pInfoDict->SetNewFor<CPDF_String>("CreationDate", DateStr);
    }
    pInfoDict->SetNewFor<CPDF_String>("Creator", L"PDFium");
  }

  // Caller takes ownership of pDoc.
  return FPDFDocumentFromCPDFDocument(pDoc.release());
}

FPDF_EXPORT void FPDF_CALLCONV FPDFPage_Delete(FPDF_DOCUMENT document,
                                               int page_index) {
  auto* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return;
  }

  CPDF_Document::Extension* pExtension = pDoc->GetExtension();
  const uint32_t page_obj_num = pExtension ? pExtension->DeletePage(page_index)
                                           : pDoc->DeletePage(page_index);
  pDoc->SetPageToNullObject(page_obj_num);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_MovePages(FPDF_DOCUMENT document,
               const int* page_indices,
               unsigned long page_indices_len,
               int dest_page_index) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return false;
  }

  // SAFETY: caller ensures `page_indices` points to at least
  // `page_indices_len` ints.
  return doc->MovePages(
      UNSAFE_BUFFERS(pdfium::span(page_indices, page_indices_len)),
      dest_page_index);
}

FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV FPDFPage_New(FPDF_DOCUMENT document,
                                                 int page_index,
                                                 double width,
                                                 double height) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return nullptr;
  }

  page_index = std::clamp(page_index, 0, pDoc->GetPageCount());
  RetainPtr<CPDF_Dictionary> pPageDict(pDoc->CreateNewPage(page_index));
  if (!pPageDict) {
    return nullptr;
  }

  pPageDict->SetRectFor(pdfium::page_object::kMediaBox,
                        CFX_FloatRect(0, 0, width, height));
  pPageDict->SetNewFor<CPDF_Number>(pdfium::page_object::kRotate, 0);
  pPageDict->SetNewFor<CPDF_Dictionary>(pdfium::page_object::kResources);

#ifdef PDF_ENABLE_XFA
  if (pDoc->GetExtension()) {
    auto pXFAPage = pdfium::MakeRetain<CPDFXFA_Page>(pDoc, page_index);
    pXFAPage->LoadPDFPageFromDict(pPageDict);
    return FPDFPageFromIPDFPage(pXFAPage.Leak());  // Caller takes ownership.
  }
#endif  // PDF_ENABLE_XFA

  auto pPage = pdfium::MakeRetain<CPDF_Page>(pDoc, pPageDict);
  pPage->AddPageImageCache();
  pPage->ParseContent();

  return FPDFPageFromIPDFPage(pPage.Leak());  // Caller takes ownership.
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetRotation(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  return IsPageObject(pPage) ? pPage->GetPageRotation() : -1;
}

FPDF_EXPORT void FPDF_CALLCONV
FPDFPage_InsertObject(FPDF_PAGE page, FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return;
  }

  std::unique_ptr<CPDF_PageObject> pPageObjHolder(pPageObj);
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(pPage)) {
    return;
  }

  pPageObj->SetDirty(true);
  pPage->AppendPageObject(std::move(pPageObjHolder));
  CalcBoundingBox(pPageObj);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_InsertObjectAtIndex(FPDF_PAGE page,
                             FPDF_PAGEOBJECT page_object,
                             size_t index) {
  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object) {
    return false;
  }

  // Take ownership back from the embedder across the C API.
  std::unique_ptr<CPDF_PageObject> page_obj_holder(cpage_object);

  CPDF_Page* cpage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(cpage)) {
    return false;
  }

  cpage_object->SetDirty(true);
  CalcBoundingBox(cpage_object);

  return cpage->InsertPageObjectAtIndex(index, std::move(page_obj_holder));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_RemoveObject(FPDF_PAGE page, FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(pPage)) {
    return false;
  }

  // Release ownership to the caller.
  return !!pPage->RemovePageObject(pPageObj).release();
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_CountObjects(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(pPage)) {
    return -1;
  }

  return pdfium::checked_cast<int>(pPage->GetPageObjectCount());
}

FPDF_EXPORT FPDF_PAGEOBJECT FPDF_CALLCONV FPDFPage_GetObject(FPDF_PAGE page,
                                                             int index) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(pPage)) {
    return nullptr;
  }

  return FPDFPageObjectFromCPDFPageObject(pPage->GetPageObjectByIndex(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_HasTransparency(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  return pPage && pPage->BackgroundAlphaNeeded();
}

FPDF_EXPORT void FPDF_CALLCONV
FPDFPageObj_Destroy(FPDF_PAGEOBJECT page_object) {
  delete CPDFPageObjectFromFPDFPageObject(page_object);
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetMarkedContentID(FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object) {
    return -1;
  }

  return cpage_object->GetContentMarks()->GetMarkedContentID();
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_CountMarks(FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return -1;
  }

  return pdfium::checked_cast<int>(pPageObj->GetContentMarks()->CountItems());
}

FPDF_EXPORT FPDF_PAGEOBJECTMARK FPDF_CALLCONV
FPDFPageObj_GetMark(FPDF_PAGEOBJECT page_object, unsigned long index) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return nullptr;
  }

  CPDF_ContentMarks* pMarks = pPageObj->GetContentMarks();
  if (index >= pMarks->CountItems()) {
    return nullptr;
  }

  return FPDFPageObjectMarkFromCPDFContentMarkItem(pMarks->GetItem(index));
}

FPDF_EXPORT FPDF_PAGEOBJECTMARK FPDF_CALLCONV
FPDFPageObj_AddMark(FPDF_PAGEOBJECT page_object, FPDF_BYTESTRING name) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return nullptr;
  }

  CPDF_ContentMarks* pMarks = pPageObj->GetContentMarks();
  pMarks->AddMark(name);
  pPageObj->SetDirty(true);

  const size_t index = pMarks->CountItems() - 1;
  return FPDFPageObjectMarkFromCPDFContentMarkItem(pMarks->GetItem(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_RemoveMark(FPDF_PAGEOBJECT page_object, FPDF_PAGEOBJECTMARK mark) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  CPDF_ContentMarkItem* pMarkItem =
      CPDFContentMarkItemFromFPDFPageObjectMark(mark);
  if (!pPageObj || !pMarkItem) {
    return false;
  }

  if (!pPageObj->GetContentMarks()->RemoveMark(pMarkItem)) {
    return false;
  }

  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_GetName(FPDF_PAGEOBJECTMARK mark,
                        FPDF_WCHAR* buffer,
                        unsigned long buflen,
                        unsigned long* out_buflen) {
  const CPDF_ContentMarkItem* pMarkItem =
      CPDFContentMarkItemFromFPDFPageObjectMark(mark);
  if (!pMarkItem || !out_buflen) {
    return false;
  }
  // SAFETY: required from caller.
  *out_buflen = Utf16EncodeMaybeCopyAndReturnLength(
      WideString::FromUTF8(pMarkItem->GetName().AsStringView()),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObjMark_CountParams(FPDF_PAGEOBJECTMARK mark) {
  const CPDF_ContentMarkItem* pMarkItem =
      CPDFContentMarkItemFromFPDFPageObjectMark(mark);
  if (!pMarkItem) {
    return -1;
  }

  RetainPtr<const CPDF_Dictionary> pParams = pMarkItem->GetParam();
  return pParams ? fxcrt::CollectionSize<int>(*pParams) : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_GetParamKey(FPDF_PAGEOBJECTMARK mark,
                            unsigned long index,
                            FPDF_WCHAR* buffer,
                            unsigned long buflen,
                            unsigned long* out_buflen) {
  if (!out_buflen) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> pParams = GetMarkParamDict(mark);
  if (!pParams) {
    return false;
  }

  CPDF_DictionaryLocker locker(pParams);
  for (auto& it : locker) {
    if (index == 0) {
      // SAFETY: required from caller.
      *out_buflen = Utf16EncodeMaybeCopyAndReturnLength(
          WideString::FromUTF8(it.first.AsStringView()),
          UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
      return true;
    }
    --index;
  }

  return false;
}

FPDF_EXPORT FPDF_OBJECT_TYPE FPDF_CALLCONV
FPDFPageObjMark_GetParamValueType(FPDF_PAGEOBJECTMARK mark,
                                  FPDF_BYTESTRING key) {
  RetainPtr<const CPDF_Dictionary> pParams = GetMarkParamDict(mark);
  if (!pParams) {
    return FPDF_OBJECT_UNKNOWN;
  }

  RetainPtr<const CPDF_Object> pObject = pParams->GetObjectFor(key);
  return pObject ? pObject->GetType() : FPDF_OBJECT_UNKNOWN;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_GetParamIntValue(FPDF_PAGEOBJECTMARK mark,
                                 FPDF_BYTESTRING key,
                                 int* out_value) {
  if (!out_value) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> pParams = GetMarkParamDict(mark);
  if (!pParams) {
    return false;
  }

  RetainPtr<const CPDF_Object> pObj = pParams->GetObjectFor(key);
  if (!pObj || !pObj->IsNumber()) {
    return false;
  }

  *out_value = pObj->GetInteger();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_GetParamStringValue(FPDF_PAGEOBJECTMARK mark,
                                    FPDF_BYTESTRING key,
                                    FPDF_WCHAR* buffer,
                                    unsigned long buflen,
                                    unsigned long* out_buflen) {
  if (!out_buflen) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> pParams = GetMarkParamDict(mark);
  if (!pParams) {
    return false;
  }

  RetainPtr<const CPDF_Object> pObj = pParams->GetObjectFor(key);
  if (!pObj || !pObj->IsString()) {
    return false;
  }

  // SAFETY: required from caller.
  *out_buflen = Utf16EncodeMaybeCopyAndReturnLength(
      WideString::FromUTF8(pObj->GetString().AsStringView()),
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_GetParamBlobValue(FPDF_PAGEOBJECTMARK mark,
                                  FPDF_BYTESTRING key,
                                  unsigned char* buffer,
                                  unsigned long buflen,
                                  unsigned long* out_buflen) {
  if (!out_buflen) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> pParams = GetMarkParamDict(mark);
  if (!pParams) {
    return false;
  }

  RetainPtr<const CPDF_Object> pObj = pParams->GetObjectFor(key);
  if (!pObj || !pObj->IsString()) {
    return false;
  }

  // SAFETY: required from caller.
  auto result_span = UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen));
  ByteString value = pObj->GetString();
  fxcrt::try_spancpy(result_span, value.span());
  *out_buflen = pdfium::checked_cast<unsigned long>(value.span().size());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_HasTransparency(FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }
  if (pPageObj->general_state().GetBlendType() != BlendMode::kNormal) {
    return true;
  }
  if (pPageObj->general_state().GetSoftMask()) {
    return true;
  }
  if (pPageObj->general_state().GetFillAlpha() != 1.0f) {
    return true;
  }
  if (pPageObj->IsPath() &&
      pPageObj->general_state().GetStrokeAlpha() != 1.0f) {
    return true;
  }
  if (!pPageObj->IsForm()) {
    return false;
  }

  const CPDF_Form* pForm = pPageObj->AsForm()->form();
  if (!pForm) {
    return false;
  }

  const CPDF_Transparency& trans = pForm->GetTransparency();
  return trans.IsGroup() || trans.IsIsolated();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_SetIntParam(FPDF_DOCUMENT document,
                            FPDF_PAGEOBJECT page_object,
                            FPDF_PAGEOBJECTMARK mark,
                            FPDF_BYTESTRING key,
                            int value) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !PageObjectContainsMark(pPageObj, mark)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pParams =
      GetOrCreateMarkParamsDict(document, mark);
  if (!pParams) {
    return false;
  }

  pParams->SetNewFor<CPDF_Number>(key, value);
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_SetStringParam(FPDF_DOCUMENT document,
                               FPDF_PAGEOBJECT page_object,
                               FPDF_PAGEOBJECTMARK mark,
                               FPDF_BYTESTRING key,
                               FPDF_BYTESTRING value) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !PageObjectContainsMark(pPageObj, mark)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pParams =
      GetOrCreateMarkParamsDict(document, mark);
  if (!pParams) {
    return false;
  }

  pParams->SetNewFor<CPDF_String>(key, value);
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_SetBlobParam(FPDF_DOCUMENT document,
                             FPDF_PAGEOBJECT page_object,
                             FPDF_PAGEOBJECTMARK mark,
                             FPDF_BYTESTRING key,
                             const unsigned char* value,
                             unsigned long value_len) {
  if (!value && value_len > 0) {
    return false;
  }

  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !PageObjectContainsMark(pPageObj, mark)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pParams =
      GetOrCreateMarkParamsDict(document, mark);
  if (!pParams) {
    return false;
  }

  // SAFETY: required from caller.
  pParams->SetNewFor<CPDF_String>(
      key, UNSAFE_BUFFERS(pdfium::span(value, value_len)),
      CPDF_String::DataType::kIsHex);
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObjMark_RemoveParam(FPDF_PAGEOBJECT page_object,
                            FPDF_PAGEOBJECTMARK mark,
                            FPDF_BYTESTRING key) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pParams = GetMarkParamDict(mark);
  if (!pParams) {
    return false;
  }

  auto removed = pParams->RemoveFor(key);
  if (!removed) {
    return false;
  }

  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPageObj_GetType(FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  return pPageObj ? static_cast<int>(pPageObj->GetType())
                  : FPDF_PAGEOBJ_UNKNOWN;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetIsActive(FPDF_PAGEOBJECT page_object, FPDF_BOOL* active) {
  if (!active) {
    return false;
  }

  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object) {
    return false;
  }

  *active = cpage_object->IsActive();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetIsActive(FPDF_PAGEOBJECT page_object, FPDF_BOOL active) {
  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object) {
    return false;
  }

  cpage_object->SetIsActive(active);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_GenerateContent(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(pPage)) {
    return false;
  }

  CPDF_PageContentGenerator CG(pPage);
  CG.GenerateContent();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedVectorProbe(FPDF_DOCUMENT document,
                                   FPDF_PAGE page,
                                   const uint8_t* stream_data,
                                   unsigned long stream_size) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!stream_data || stream_size == 0) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  return ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewWrappedContentStream(doc, stream_data, stream_size);
      });
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedVectorProbeWithExtGState(FPDF_DOCUMENT document,
                                                FPDF_PAGE page,
                                                const uint8_t* stream_data,
                                                unsigned long stream_size,
                                                float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!stream_data || stream_size == 0 || !std::isfinite(alpha) ||
      alpha < 0.0f || alpha > 1.0f) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  if (!ext_gstate_resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate = NewExtGState(doc, alpha);
  ByteString resource_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(ext_gstate_resources,
                                                          "GS");
  if (resource_name.IsEmpty()) {
    return false;
  }
  ext_gstate_resources->SetNewFor<CPDF_Reference>(resource_name, doc,
                                                  ext_gstate->GetObjNum());

  return ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewWrappedExtGStateContentStream(
            doc, resource_name.AsStringView(), stream_data, stream_size);
      });
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedTextProbeWithStandardFont(FPDF_DOCUMENT document,
                                                 FPDF_PAGE page,
                                                 const char* text,
                                                 unsigned long text_size,
                                                 float x,
                                                 float y,
                                                 float font_size,
                                                 float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(font_size) ||
      !std::isfinite(alpha) || font_size <= 0.0f || alpha < 0.0f ||
      alpha > 1.0f) {
    return false;
  }

  ByteString escaped_text;
  if (!EscapeAsciiLiteralString(text, text_size, &escaped_text)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> font_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "Font");
  if (!ext_gstate_resources || !font_resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate = NewExtGState(doc, alpha);
  ByteString gs_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(ext_gstate_resources,
                                                          "GS");
  if (gs_name.IsEmpty()) {
    return false;
  }
  ext_gstate_resources->SetNewFor<CPDF_Reference>(gs_name, doc,
                                                  ext_gstate->GetObjNum());

  RetainPtr<CPDF_Dictionary> font = NewStandardHelveticaFont(doc);
  ByteString font_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(font_resources, "F");
  if (font_name.IsEmpty()) {
    return false;
  }
  font_resources->SetNewFor<CPDF_Reference>(font_name, doc, font->GetObjNum());

  return ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewWrappedTextProbeContentStream(
            doc, gs_name.AsStringView(), font_name.AsStringView(),
            escaped_text.AsStringView(), x, y, font_size);
      });
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedUnicodeTextProbeWithEmbeddedFont(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    const uint8_t* font_data,
    uint32_t font_data_size,
    FPDF_WIDESTRING text,
    float x,
    float y,
    float font_size,
    float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!font_data || font_data_size == 0 || !text || !std::isfinite(x) ||
      !std::isfinite(y) || !std::isfinite(font_size) ||
      !std::isfinite(alpha) || font_size <= 0.0f || alpha < 0.0f ||
      alpha > 1.0f) {
    return false;
  }

  // SAFETY: The public API contract requires `text` to be NUL-terminated.
  WideString wide_text = UNSAFE_BUFFERS(WideStringFromFPDFWideString(text));
  if (wide_text.IsEmpty()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  FPDF_FONT font_handle = FPDFText_LoadFont(document, font_data, font_data_size,
                                            FPDF_FONT_TRUETYPE, /*cid=*/true);
  if (!font_handle) {
    return false;
  }

  CPDF_Font* font = CPDFFontFromFPDFFont(font_handle);
  if (!font) {
    FPDFFont_Close(font_handle);
    return false;
  }

  ByteString encoded_text;
  if (!EncodeUnicodeTextWithFont(wide_text, font, &encoded_text)) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> font_dict = font->GetMutableFontDict();
  if (!font_dict) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> font_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "Font");
  if (!ext_gstate_resources || !font_resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  ByteString gs_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(ext_gstate_resources,
                                                          "GS");
  ByteString font_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(font_resources, "F");
  if (gs_name.IsEmpty() || font_name.IsEmpty() || font_dict->GetObjNum() == 0) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate = NewExtGState(doc, alpha);
  ext_gstate_resources->SetNewFor<CPDF_Reference>(gs_name, doc,
                                                  ext_gstate->GetObjNum());
  font_resources->SetNewFor<CPDF_Reference>(font_name, doc,
                                            font_dict->GetObjNum());

  bool append_result = ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewWrappedUnicodeTextProbeContentStream(
            doc, gs_name.AsStringView(), font_name.AsStringView(),
            encoded_text.AsStringView(), x, y, font_size);
      });

  FPDFFont_Close(font_handle);
  return append_result;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedUnicodeTextObjectProbeWithEmbeddedFont(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    const uint8_t* font_data,
    uint32_t font_data_size,
    FPDF_WIDESTRING text,
    float x,
    float y,
    float font_size,
    float rotation_degrees,
    float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!font_data || font_data_size == 0 || !text || !std::isfinite(x) ||
      !std::isfinite(y) || !std::isfinite(font_size) ||
      !std::isfinite(rotation_degrees) || !std::isfinite(alpha) ||
      font_size <= 0.0f || alpha < 0.0f || alpha > 1.0f) {
    return false;
  }

  // SAFETY: The public API contract requires `text` to be NUL-terminated.
  WideString wide_text = UNSAFE_BUFFERS(WideStringFromFPDFWideString(text));
  if (wide_text.IsEmpty()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  FPDF_FONT font_handle = FPDFText_LoadFont(document, font_data, font_data_size,
                                            FPDF_FONT_TRUETYPE, /*cid=*/true);
  if (!font_handle) {
    return false;
  }

  CPDF_Font* font = CPDFFontFromFPDFFont(font_handle);
  if (!font) {
    FPDFFont_Close(font_handle);
    return false;
  }

  ByteString encoded_text;
  if (!EncodeUnicodeTextWithFont(wide_text, font, &encoded_text)) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> font_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "Font");
  if (!ext_gstate_resources || !font_resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  CPDF_TextObject text_object;
  text_object.SetDefaultStates();
  text_object.mutable_text_state().SetFont(pdfium::WrapRetain(font));
  text_object.mutable_text_state().SetFontSize(font_size);
  text_object.SetText(encoded_text);

  constexpr float kPi = 3.14159265358979323846f;
  const float radians = rotation_degrees * kPi / 180.0f;
  const float cos_theta = std::cos(radians);
  const float sin_theta = std::sin(radians);
  text_object.SetTextMatrix(
      CFX_Matrix(cos_theta, sin_theta, -sin_theta, cos_theta, x, y));

  std::vector<float> red = {1.0f, 0.0f, 0.0f};
  text_object.mutable_color_state().SetFillColor(
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB),
      std::move(red));
  text_object.mutable_general_state().SetFillAlpha(alpha);
  text_object.mutable_general_state().SetStrokeAlpha(alpha);

  CPDF_PageContentGenerator generator(pdf_page);
  ByteString generated_stream =
      generator.GenerateAppendOnlyTextObjectStream(&text_object);
  if (generated_stream.IsEmpty()) {
    FPDFFont_Close(font_handle);
    return false;
  }

  bool append_result = ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewContentStream(doc, generated_stream.AsStringView());
      });

  FPDFFont_Close(font_handle);
  return append_result;
}

struct ReusableTextStampParams {
  FPDF_DOCUMENT document;
  FPDF_PAGE page;
  FPDF_WIDESTRING text;
  float stamp_width;
  float stamp_height;
  float text_x;
  float text_y;
  float font_size;
  const std::vector<CFX_Matrix>& placements;
  unsigned int fill_R;
  unsigned int fill_G;
  unsigned int fill_B;
  float alpha;
};

struct ReusableTextStampPreflight {
  CPDF_Document* document;
  CPDF_Page* page;
  RetainPtr<CPDF_Dictionary> page_dict;
  WideString text;
  std::vector<uint32_t> old_content_object_numbers;
};

std::optional<ReusableTextStampPreflight> PrepareReusableTextStamp(
    const ReusableTextStampParams& params) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(params.document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(params.page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return std::nullopt;
  }

  if (!params.text || params.placements.empty() ||
      !std::isfinite(params.stamp_width) ||
      !std::isfinite(params.stamp_height) ||
      !std::isfinite(params.text_x) || !std::isfinite(params.text_y) ||
      !std::isfinite(params.font_size) || !std::isfinite(params.alpha) ||
      params.stamp_width <= 0.0f || params.stamp_height <= 0.0f ||
      params.font_size <= 0.0f || params.fill_R > 255 ||
      params.fill_G > 255 || params.fill_B > 255 || params.alpha < 0.0f ||
      params.alpha > 1.0f) {
    return std::nullopt;
  }

  for (const CFX_Matrix& placement : params.placements) {
    if (!IsValidPlacementMatrix(placement)) {
      return std::nullopt;
    }
  }

  // SAFETY: The public API contract requires `text` to be NUL-terminated.
  WideString wide_text =
      UNSAFE_BUFFERS(WideStringFromFPDFWideString(params.text));
  if (wide_text.IsEmpty()) {
    return std::nullopt;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return std::nullopt;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return std::nullopt;
  }

  return ReusableTextStampPreflight{
      doc, pdf_page, std::move(page_dict), std::move(wide_text),
      std::move(old_content_object_numbers)};
}

bool AppendReusableUnicodeTextStampXObjectWithPlacementsInternal(
    FPDF_FONT font_handle,
    ReusableTextStampPreflight preflight,
    const ReusableTextStampParams& params) {
  if (!font_handle) {
    return false;
  }

  CPDF_Document* doc = preflight.document;
  CPDF_Page* pdf_page = preflight.page;

  CPDF_Font* font = CPDFFontFromFPDFFont(font_handle);
  if (!font) {
    FPDFFont_Close(font_handle);
    return false;
  }

  ByteString encoded_text;
  if (!EncodeUnicodeTextWithFont(preflight.text, font, &encoded_text)) {
    FPDFFont_Close(font_handle);
    return false;
  }

  auto form_dict = doc->New<CPDF_Dictionary>();
  form_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  form_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  form_dict->SetNewFor<CPDF_Number>("FormType", 1);
  form_dict->SetRectFor(
      "BBox", CFX_FloatRect(0, 0, params.stamp_width, params.stamp_height));
  form_dict->SetMatrixFor("Matrix", CFX_Matrix());
  RetainPtr<CPDF_Dictionary> form_resources =
      form_dict->SetNewFor<CPDF_Dictionary>("Resources");
  RetainPtr<CPDF_Stream> form_stream =
      doc->NewIndirect<CPDF_Stream>(std::move(form_dict));
  if (!form_stream || !form_resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  RetainPtr<CPDF_Dictionary> form_ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, form_resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> form_font_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, form_resources,
                                                          "Font");
  if (!form_ext_gstate_resources || !form_font_resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  CPDF_Form form_holder(doc, RetainPtr<CPDF_Dictionary>(), form_stream);
  if (form_holder.GetMutableResources().Get() != form_resources.Get()) {
    FPDFFont_Close(font_handle);
    return false;
  }

  CPDF_TextObject text_object;
  text_object.SetDefaultStates();
  text_object.mutable_text_state().SetFont(pdfium::WrapRetain(font));
  text_object.mutable_text_state().SetFontSize(params.font_size);
  text_object.SetText(encoded_text);
  text_object.SetTextMatrix(
      CFX_Matrix(1, 0, 0, 1, params.text_x, params.text_y));

  std::vector<float> fill_color = {params.fill_R / 255.f,
                                   params.fill_G / 255.f,
                                   params.fill_B / 255.f};
  text_object.mutable_color_state().SetFillColor(
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB),
      std::move(fill_color));
  text_object.mutable_general_state().SetFillAlpha(params.alpha);
  text_object.mutable_general_state().SetStrokeAlpha(params.alpha);

  CPDF_PageContentGenerator form_generator(&form_holder);
  ByteString form_content =
      form_generator.GenerateAppendOnlyTextObjectStream(&text_object);
  if (form_content.IsEmpty()) {
    FPDFFont_Close(font_handle);
    return false;
  }
  form_stream->SetData(form_content.unsigned_span());

  RetainPtr<CPDF_Dictionary> page_resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!page_resources) {
    FPDFFont_Close(font_handle);
    return false;
  }
  RetainPtr<CPDF_Dictionary> xobject_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, page_resources,
                                                          "XObject");
  if (!xobject_resources) {
    FPDFFont_Close(font_handle);
    return false;
  }

  ByteString stamp_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(xobject_resources,
                                                          "WM");
  if (stamp_name.IsEmpty()) {
    FPDFFont_Close(font_handle);
    return false;
  }
  xobject_resources->SetNewFor<CPDF_Reference>(stamp_name, doc,
                                               form_stream->GetObjNum());

  bool append_result = ReplacePageContentsWithIsolatedAppendStream(
      doc, preflight.page_dict, preflight.old_content_object_numbers,
      [&]() {
        return NewReusableTextStampPlacementContentStream(
            doc, stamp_name.AsStringView(), params.placements);
      });

  FPDFFont_Close(font_handle);
  return append_result;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendReusableUnicodeTextStampXObjectWithEmbeddedFontProbe(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    const uint8_t* font_data,
    uint32_t font_data_size,
    FPDF_WIDESTRING text,
    float stamp_width,
    float stamp_height,
    float text_x,
    float text_y,
    float font_size,
    const FS_MATRIX* placements,
    uint32_t placement_count,
    unsigned int fill_R,
    unsigned int fill_G,
    unsigned int fill_B,
    float alpha) {
  if (!placements || placement_count == 0) {
    return false;
  }

  std::vector<CFX_Matrix> placement_matrices;
  placement_matrices.reserve(placement_count);
  for (uint32_t i = 0; i < placement_count; ++i) {
    placement_matrices.push_back(CFXMatrixFromFSMatrix(placements[i]));
  }

  ReusableTextStampParams params = {
      document,     page,   text,   stamp_width, stamp_height,
      text_x,       text_y, font_size, placement_matrices,
      fill_R,       fill_G, fill_B, alpha};
  std::optional<ReusableTextStampPreflight> preflight =
      PrepareReusableTextStamp(params);
  if (!preflight.has_value()) {
    return false;
  }

  FPDF_FONT font_handle =
      LoadReusableTextStampEmbeddedFont(document, font_data, font_data_size);

  return AppendReusableUnicodeTextStampXObjectWithPlacementsInternal(
      font_handle, std::move(*preflight), params);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendReusableUnicodeTextStampXObjectWithStandardFontProbe(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    FPDF_BYTESTRING standard_font_name,
    FPDF_WIDESTRING text,
    float stamp_width,
    float stamp_height,
    float text_x,
    float text_y,
    float font_size,
    const FS_MATRIX* placements,
    uint32_t placement_count,
    unsigned int fill_R,
    unsigned int fill_G,
    unsigned int fill_B,
    float alpha) {
  if (!placements || placement_count == 0) {
    return false;
  }

  std::vector<CFX_Matrix> placement_matrices;
  placement_matrices.reserve(placement_count);
  for (uint32_t i = 0; i < placement_count; ++i) {
    placement_matrices.push_back(CFXMatrixFromFSMatrix(placements[i]));
  }

  ReusableTextStampParams params = {
      document,     page,   text,   stamp_width, stamp_height,
      text_x,       text_y, font_size, placement_matrices,
      fill_R,       fill_G, fill_B, alpha};
  std::optional<ReusableTextStampPreflight> preflight =
      PrepareReusableTextStamp(params);
  if (!preflight.has_value()) {
    return false;
  }

  FPDF_FONT font_handle =
      LoadReusableTextStampStandardFont(document, standard_font_name);

  return AppendReusableUnicodeTextStampXObjectWithPlacementsInternal(
      font_handle, std::move(*preflight), params);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedImageProbeWithXObject(FPDF_DOCUMENT document,
                                             FPDF_PAGE page,
                                             const uint8_t* rgb_data,
                                             unsigned long rgb_size,
                                             int image_width,
                                             int image_height,
                                             float x,
                                             float y,
                                             float draw_width,
                                             float draw_height,
                                             float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!rgb_data ||
      !IsValidRawDeviceRgbSize(image_width, image_height, rgb_size) ||
      !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(draw_width) || !std::isfinite(draw_height) ||
      !std::isfinite(alpha) || draw_width <= 0.0f ||
      draw_height <= 0.0f || alpha < 0.0f || alpha > 1.0f) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> xobject_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "XObject");
  if (!ext_gstate_resources || !xobject_resources) {
    return false;
  }

  ByteString gs_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(ext_gstate_resources,
                                                          "GS");
  ByteString image_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(xobject_resources,
                                                          "Im");
  if (gs_name.IsEmpty() || image_name.IsEmpty()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate = NewExtGState(doc, alpha);
  ext_gstate_resources->SetNewFor<CPDF_Reference>(gs_name, doc,
                                                  ext_gstate->GetObjNum());

  RetainPtr<CPDF_Stream> image_xobject = NewRawDeviceRgbImageXObject(
      doc, rgb_data, rgb_size, image_width, image_height);
  xobject_resources->SetNewFor<CPDF_Reference>(image_name, doc,
                                               image_xobject->GetObjNum());

  return ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewWrappedImageProbeContentStream(
            doc, gs_name.AsStringView(), image_name.AsStringView(), x, y,
            draw_width, draw_height);
      });
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendIsolatedRgbaImageProbeWithXObject(FPDF_DOCUMENT document,
                                                 FPDF_PAGE page,
                                                 const uint8_t* rgba_data,
                                                 unsigned long rgba_size,
                                                 int image_width,
                                                 int image_height,
                                                 float x,
                                                 float y,
                                                 float draw_width,
                                                 float draw_height,
                                                 float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!rgba_data ||
      !IsValidRawDeviceRgbaSize(image_width, image_height, rgba_size) ||
      !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(draw_width) || !std::isfinite(draw_height) ||
      !std::isfinite(alpha) || draw_width <= 0.0f ||
      draw_height <= 0.0f || alpha < 0.0f || alpha > 1.0f) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> xobject_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "XObject");
  if (!ext_gstate_resources || !xobject_resources) {
    return false;
  }

  ByteString gs_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(ext_gstate_resources,
                                                          "GS");
  ByteString image_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(xobject_resources,
                                                          "Im");
  if (gs_name.IsEmpty() || image_name.IsEmpty()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate = NewExtGState(doc, alpha);
  ext_gstate_resources->SetNewFor<CPDF_Reference>(gs_name, doc,
                                                  ext_gstate->GetObjNum());

  RetainPtr<CPDF_Stream> image_xobject = NewRawDeviceRgbaImageXObject(
      doc, rgba_data, rgba_size, image_width, image_height);
  xobject_resources->SetNewFor<CPDF_Reference>(image_name, doc,
                                               image_xobject->GetObjNum());

  return ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewWrappedImageProbeContentStream(
            doc, gs_name.AsStringView(), image_name.AsStringView(), x, y,
            draw_width, draw_height);
      });
}

FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFImageObj_CreateReusableRgbaImageXObjectProbe(FPDF_DOCUMENT document,
                                                 const uint8_t* rgba_data,
                                                 unsigned long rgba_size,
                                                 int image_width,
                                                 int image_height) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || !rgba_data ||
      !IsValidRawDeviceRgbaSize(image_width, image_height, rgba_size)) {
    return 0;
  }

  RetainPtr<CPDF_Stream> image_xobject = NewRawDeviceRgbaImageXObject(
      doc, rgba_data, rgba_size, image_width, image_height);
  return image_xobject ? image_xobject->GetObjNum() : 0;
}

bool AppendReusableImageXObjectWithPlacementsInternal(
    FPDF_DOCUMENT document,
    FPDF_PAGE page,
    uint32_t image_object_number,
    const std::vector<CFX_Matrix>& placements,
    float alpha) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page);
  if (!doc || !IsPageObject(pdf_page) || pdf_page->GetDocument() != doc) {
    return false;
  }
  if (!IsReusableImageXObject(doc, image_object_number) || placements.empty() ||
      !std::isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
    return false;
  }

  for (const CFX_Matrix& placement : placements) {
    if (!IsValidPlacementMatrix(placement)) {
      return false;
    }
  }

  RetainPtr<CPDF_Dictionary> page_dict = pdf_page->GetMutableDict();
  if (!page_dict) {
    return false;
  }

  RetainPtr<CPDF_Object> old_contents =
      page_dict->GetMutableObjectFor(pdfium::page_object::kContents);
  std::vector<uint32_t> old_content_object_numbers;
  if (!CollectOriginalContentRefs(old_contents, &old_content_object_numbers)) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> resources =
      CPDF_PageResourceEditor::EnsurePageLocalResources(doc, pdf_page);
  if (!resources) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "ExtGState");
  RetainPtr<CPDF_Dictionary> xobject_resources =
      CPDF_PageResourceEditor::EnsureLocalResourceSubdict(doc, resources,
                                                          "XObject");
  if (!ext_gstate_resources || !xobject_resources) {
    return false;
  }

  ByteString gs_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(ext_gstate_resources,
                                                          "GS");
  ByteString image_name =
      CPDF_PageResourceEditor::AllocateUniqueResourceName(xobject_resources,
                                                          "Im");
  if (gs_name.IsEmpty() || image_name.IsEmpty()) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> ext_gstate = NewExtGState(doc, alpha);
  ext_gstate_resources->SetNewFor<CPDF_Reference>(gs_name, doc,
                                                  ext_gstate->GetObjNum());
  xobject_resources->SetNewFor<CPDF_Reference>(image_name, doc,
                                               image_object_number);

  return ReplacePageContentsWithIsolatedAppendStream(
      doc, page_dict, old_content_object_numbers,
      [&]() {
        return NewReusableImageXObjectPlacementContentStream(
            doc, gs_name.AsStringView(), image_name.AsStringView(),
            placements);
      });
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFPage_AppendReusableImageXObjectProbe(FPDF_DOCUMENT document,
                                         FPDF_PAGE page,
                                         uint32_t image_object_number,
                                         const FS_MATRIX* placements,
                                         uint32_t placement_count,
                                         float alpha) {
  if (!placements || placement_count == 0) {
    return false;
  }

  std::vector<CFX_Matrix> placement_matrices;
  placement_matrices.reserve(placement_count);
  for (uint32_t i = 0; i < placement_count; ++i) {
    placement_matrices.push_back(CFXMatrixFromFSMatrix(placements[i]));
  }

  return AppendReusableImageXObjectWithPlacementsInternal(
      document, page, image_object_number, placement_matrices, alpha);
}


FPDF_EXPORT void FPDF_CALLCONV
FPDFPageObj_Transform(FPDF_PAGEOBJECT page_object,
                      double a,
                      double b,
                      double c,
                      double d,
                      double e,
                      double f) {
  const FS_MATRIX matrix{static_cast<float>(a), static_cast<float>(b),
                         static_cast<float>(c), static_cast<float>(d),
                         static_cast<float>(e), static_cast<float>(f)};
  FPDFPageObj_TransformF(page_object, &matrix);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_TransformF(FPDF_PAGEOBJECT page_object, const FS_MATRIX* matrix) {
  if (!matrix) {
    return false;
  }

  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object) {
    return false;
  }

  cpage_object->Transform(CFXMatrixFromFSMatrix(*matrix));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetMatrix(FPDF_PAGEOBJECT page_object, FS_MATRIX* matrix) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !matrix) {
    return false;
  }

  switch (pPageObj->GetType()) {
    case CPDF_PageObject::Type::kText:
      *matrix = FSMatrixFromCFXMatrix(pPageObj->AsText()->GetTextMatrix());
      return true;
    case CPDF_PageObject::Type::kPath:
      *matrix = FSMatrixFromCFXMatrix(pPageObj->AsPath()->matrix());
      return true;
    case CPDF_PageObject::Type::kImage:
      *matrix = FSMatrixFromCFXMatrix(pPageObj->AsImage()->matrix());
      return true;
    case CPDF_PageObject::Type::kShading:
      return false;
    case CPDF_PageObject::Type::kForm:
      *matrix = FSMatrixFromCFXMatrix(pPageObj->AsForm()->form_matrix());
      return true;
  }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetMatrix(FPDF_PAGEOBJECT page_object, const FS_MATRIX* matrix) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !matrix) {
    return false;
  }

  CFX_Matrix cmatrix = CFXMatrixFromFSMatrix(*matrix);
  switch (pPageObj->GetType()) {
    case CPDF_PageObject::Type::kText:
      pPageObj->AsText()->SetTextMatrix(cmatrix);
      pPageObj->SetMatrixDirty(true);
      return true;
    case CPDF_PageObject::Type::kPath:
      pPageObj->AsPath()->SetPathMatrix(cmatrix);
      pPageObj->SetMatrixDirty(true);
      return true;
    case CPDF_PageObject::Type::kImage:
      pPageObj->AsImage()->SetImageMatrix(cmatrix);
      pPageObj->SetMatrixDirty(pPageObj->original_matrix() != cmatrix);
      return true;
    case CPDF_PageObject::Type::kShading:
      return false;
    case CPDF_PageObject::Type::kForm:
      pPageObj->AsForm()->SetFormMatrix(cmatrix);
      pPageObj->SetMatrixDirty(true);
      return true;
  }
  NOTREACHED();
}

FPDF_EXPORT void FPDF_CALLCONV
FPDFPageObj_SetBlendMode(FPDF_PAGEOBJECT page_object,
                         FPDF_BYTESTRING blend_mode) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return;
  }

  pPageObj->mutable_general_state().SetBlendMode(blend_mode);
  pPageObj->SetDirty(true);
}

FPDF_EXPORT void FPDF_CALLCONV FPDFPage_TransformAnnots(FPDF_PAGE page,
                                                        double a,
                                                        double b,
                                                        double c,
                                                        double d,
                                                        double e,
                                                        double f) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return;
  }

  CPDF_AnnotList AnnotList(pPage);
  for (size_t i = 0; i < AnnotList.Count(); ++i) {
    CPDF_Annot* pAnnot = AnnotList.GetAt(i);
    CFX_Matrix matrix((float)a, (float)b, (float)c, (float)d, (float)e,
                      (float)f);
    CFX_FloatRect rect = matrix.TransformRect(pAnnot->GetRect());

    RetainPtr<CPDF_Dictionary> pAnnotDict = pAnnot->GetMutableAnnotDict();
    RetainPtr<CPDF_Array> pRectArray = pAnnotDict->GetMutableArrayFor("Rect");
    if (pRectArray) {
      pRectArray->Clear();
    } else {
      pRectArray = pAnnotDict->SetNewFor<CPDF_Array>("Rect");
    }

    pRectArray->AppendNew<CPDF_Number>(rect.left);
    pRectArray->AppendNew<CPDF_Number>(rect.bottom);
    pRectArray->AppendNew<CPDF_Number>(rect.right);
    pRectArray->AppendNew<CPDF_Number>(rect.top);

    // TODO(unknown): Transform AP's rectangle
  }
}

FPDF_EXPORT void FPDF_CALLCONV FPDFPage_SetRotation(FPDF_PAGE page,
                                                    int rotate) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!IsPageObject(pPage)) {
    return;
  }

  rotate %= 4;
  pPage->GetMutableDict()->SetNewFor<CPDF_Number>(pdfium::page_object::kRotate,
                                                  rotate * 90);
  pPage->UpdateDimensions();
}

FPDF_BOOL FPDFPageObj_SetFillColor(FPDF_PAGEOBJECT page_object,
                                   unsigned int R,
                                   unsigned int G,
                                   unsigned int B,
                                   unsigned int A) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || R > 255 || G > 255 || B > 255 || A > 255) {
    return false;
  }

  std::vector<float> rgb = {R / 255.f, G / 255.f, B / 255.f};
  pPageObj->mutable_general_state().SetFillAlpha(A / 255.f);
  pPageObj->mutable_color_state().SetFillColor(
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB),
      std::move(rgb));
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetFillColor(FPDF_PAGEOBJECT page_object,
                         unsigned int* R,
                         unsigned int* G,
                         unsigned int* B,
                         unsigned int* A) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !R || !G || !B || !A) {
    return false;
  }

  if (!pPageObj->color_state().HasRef()) {
    return false;
  }

  FX_COLORREF fill_color = pPageObj->color_state().GetFillColorRef();
  *R = FXSYS_GetRValue(fill_color);
  *G = FXSYS_GetGValue(fill_color);
  *B = FXSYS_GetBValue(fill_color);
  *A = FXSYS_GetUnsignedAlpha(pPageObj->general_state().GetFillAlpha());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetBounds(FPDF_PAGEOBJECT page_object,
                      float* left,
                      float* bottom,
                      float* right,
                      float* top) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  const CFX_FloatRect& bbox = pPageObj->GetRect();
  *left = bbox.left;
  *bottom = bbox.bottom;
  *right = bbox.right;
  *top = bbox.top;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetRotatedBounds(FPDF_PAGEOBJECT page_object,
                             FS_QUADPOINTSF* quad_points) {
  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object || !quad_points) {
    return false;
  }

  CFX_Matrix matrix;
  switch (cpage_object->GetType()) {
    case CPDF_PageObject::Type::kText:
      matrix = cpage_object->AsText()->GetTextMatrix();
      break;
    case CPDF_PageObject::Type::kImage:
      matrix = cpage_object->AsImage()->matrix();
      break;
    default:
      // TODO(crbug.com/pdfium/1840): Support more object types.
      return false;
  }

  const CFX_FloatRect& bbox = cpage_object->GetOriginalRect();
  const CFX_PointF bottom_left = matrix.Transform({bbox.left, bbox.bottom});
  const CFX_PointF bottom_right = matrix.Transform({bbox.right, bbox.bottom});
  const CFX_PointF top_right = matrix.Transform({bbox.right, bbox.top});
  const CFX_PointF top_left = matrix.Transform({bbox.left, bbox.top});

  // See PDF 32000-1:2008, figure 64 for the QuadPoints ordering.
  quad_points->x1 = bottom_left.x;
  quad_points->y1 = bottom_left.y;
  quad_points->x2 = bottom_right.x;
  quad_points->y2 = bottom_right.y;
  quad_points->x3 = top_right.x;
  quad_points->y3 = top_right.y;
  quad_points->x4 = top_left.x;
  quad_points->y4 = top_left.y;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetStrokeColor(FPDF_PAGEOBJECT page_object,
                           unsigned int R,
                           unsigned int G,
                           unsigned int B,
                           unsigned int A) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || R > 255 || G > 255 || B > 255 || A > 255) {
    return false;
  }

  std::vector<float> rgb = {R / 255.f, G / 255.f, B / 255.f};
  pPageObj->mutable_general_state().SetStrokeAlpha(A / 255.f);
  pPageObj->mutable_color_state().SetStrokeColor(
      CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB),
      std::move(rgb));
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetStrokeColor(FPDF_PAGEOBJECT page_object,
                           unsigned int* R,
                           unsigned int* G,
                           unsigned int* B,
                           unsigned int* A) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !R || !G || !B || !A) {
    return false;
  }
  if (!pPageObj->color_state().HasRef()) {
    return false;
  }

  FX_COLORREF stroke_color = pPageObj->color_state().GetStrokeColorRef();
  *R = FXSYS_GetRValue(stroke_color);
  *G = FXSYS_GetGValue(stroke_color);
  *B = FXSYS_GetBValue(stroke_color);
  *A = FXSYS_GetUnsignedAlpha(pPageObj->general_state().GetStrokeAlpha());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetStrokeWidth(FPDF_PAGEOBJECT page_object, float width) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || width < 0.0f) {
    return false;
  }

  pPageObj->mutable_graph_state().SetLineWidth(width);
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetStrokeWidth(FPDF_PAGEOBJECT page_object, float* width) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !width) {
    return false;
  }

  *width = pPageObj->graph_state().GetLineWidth();
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetLineJoin(FPDF_PAGEOBJECT page_object) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  return pPageObj ? static_cast<int>(pPageObj->graph_state().GetLineJoin())
                  : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetLineJoin(FPDF_PAGEOBJECT page_object, int line_join) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  if (line_join < FPDF_LINEJOIN_MITER || line_join > FPDF_LINEJOIN_BEVEL) {
    return false;
  }

  pPageObj->mutable_graph_state().SetLineJoin(
      static_cast<CFX_GraphStateData::LineJoin>(line_join));
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetLineCap(FPDF_PAGEOBJECT page_object) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  return pPageObj ? static_cast<int>(pPageObj->graph_state().GetLineCap()) : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetLineCap(FPDF_PAGEOBJECT page_object, int line_cap) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  if (line_cap < FPDF_LINECAP_BUTT ||
      line_cap > FPDF_LINECAP_PROJECTING_SQUARE) {
    return false;
  }
  pPageObj->mutable_graph_state().SetLineCap(
      static_cast<CFX_GraphStateData::LineCap>(line_cap));
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetDashPhase(FPDF_PAGEOBJECT page_object, float* phase) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !phase) {
    return false;
  }

  *phase = pPageObj->graph_state().GetLineDashPhase();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetDashPhase(FPDF_PAGEOBJECT page_object, float phase) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  pPageObj->mutable_graph_state().SetLineDashPhase(phase);
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetDashCount(FPDF_PAGEOBJECT page_object) {
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  return pPageObj ? pdfium::checked_cast<int>(
                        pPageObj->graph_state().GetLineDashSize())
                  : -1;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_GetDashArray(FPDF_PAGEOBJECT page_object,
                         float* dash_array,
                         size_t dash_count) {
  if (!dash_array) {
    return false;
  }
  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  // SAFETY: required from caller.
  auto result_span = UNSAFE_BUFFERS(pdfium::span(dash_array, dash_count));
  auto dash_vector = pPageObj->graph_state().GetLineDashArray();
  return fxcrt::try_spancpy(result_span, pdfium::span(dash_vector));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetDashArray(FPDF_PAGEOBJECT page_object,
                         const float* dash_array,
                         size_t dash_count,
                         float phase) {
  if (dash_count > 0 && !dash_array) {
    return false;
  }

  auto* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }

  std::vector<float> dashes;
  if (dash_count > 0) {
    dashes.reserve(dash_count);
    dashes.assign(dash_array, UNSAFE_TODO(dash_array + dash_count));
  }
  pPageObj->mutable_graph_state().SetLineDash(dashes, phase);
  pPageObj->SetDirty(true);
  return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFFormObj_CountObjects(FPDF_PAGEOBJECT form_object) {
  const auto* pObjectList = CPDFPageObjHolderFromFPDFFormObject(form_object);
  return pObjectList
             ? pdfium::checked_cast<int>(pObjectList->GetPageObjectCount())
             : -1;
}

FPDF_EXPORT FPDF_PAGEOBJECT FPDF_CALLCONV
FPDFFormObj_GetObject(FPDF_PAGEOBJECT form_object, unsigned long index) {
  const auto* pObjectList = CPDFPageObjHolderFromFPDFFormObject(form_object);
  if (!pObjectList) {
    return nullptr;
  }

  return FPDFPageObjectFromCPDFPageObject(
      pObjectList->GetPageObjectByIndex(index));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFormObj_RemoveObject(FPDF_PAGEOBJECT form_object,
                         FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* cform_page_object =
      CPDFPageObjectFromFPDFPageObject(form_object);
  if (!cform_page_object) {
    return false;
  }
  CPDF_FormObject* form_obj = cform_page_object->AsForm();
  if (!form_obj) {
    return false;
  }
  CPDF_PageObject* cpage_object = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!cpage_object) {
    return false;
  }

  std::unique_ptr<CPDF_PageObject> removed_object =
      form_obj->form()->RemovePageObject(cpage_object);
  if (!removed_object) {
    return false;
  }

  cform_page_object->SetDirty(true);

  // Caller takes ownership of the removed page object
  removed_object.release();
  return true;
}
