// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "public/fpdf_edit.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "constants/page_object.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_annot.h"
#include "public/fpdf_text.h"
#include "testing/embedder_test.h"
#include "testing/embedder_test_constants.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/utils/file_util.h"
#include "testing/utils/path_service.h"

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::FloatEq;
using ::testing::Gt;

namespace {

RetainPtr<const CPDF_Dictionary> GetFormResources(CPDF_Document* document,
                                                  uint32_t object_number) {
  RetainPtr<const CPDF_Stream> form =
      ToStream(document->GetOrParseIndirectObject(object_number));
  return form ? form->GetDict()->GetDictFor("Resources") : nullptr;
}

uint32_t GetOnlyFormFontObjectNumber(CPDF_Document* document,
                                     uint32_t object_number) {
  RetainPtr<const CPDF_Dictionary> resources =
      GetFormResources(document, object_number);
  RetainPtr<const CPDF_Dictionary> fonts =
      resources ? resources->GetDictFor("Font") : nullptr;
  if (!fonts || fonts->size() != 1u) {
    return 0;
  }
  RetainPtr<const CPDF_Object> font =
      fonts->GetDirectObjectFor(fonts->GetKeys().front().AsStringView());
  return font ? font->GetObjNum() : 0;
}

bool PageReferencesForm(CPDF_Page* page, uint32_t object_number) {
  RetainPtr<const CPDF_Dictionary> resources = page->GetResources();
  RetainPtr<const CPDF_Dictionary> xobjects =
      resources ? resources->GetDictFor("XObject") : nullptr;
  if (!xobjects) {
    return false;
  }
  for (const ByteString& key : xobjects->GetKeys()) {
    RetainPtr<const CPDF_Reference> reference =
        ToReference(xobjects->GetObjectFor(key.AsStringView()));
    if (reference && reference->GetRefObjNum() == object_number) {
      return true;
    }
  }
  return false;
}

}  // namespace

class FPDFEditPageEmbedderTest : public EmbedderTest {};

TEST_F(FPDFEditPageEmbedderTest,
       ReusableTextStampPreflightDoesNotAddObjects) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ASSERT_TRUE(page);

  const std::string font_path =
      PathService::GetTestFilePath("fonts/ahem/Ahem.ttf");
  const std::vector<uint8_t> font_data = GetFileContents(font_path.c_str());
  ASSERT_FALSE(font_data.empty());

  CPDF_Document* pdf_document =
      CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf_document);
  const uint32_t font_data_size =
      pdfium::checked_cast<uint32_t>(font_data.size());
  const FS_MATRIX placement = {1, 0, 0, 1, 10, 10};
  static constexpr FPDF_WCHAR kText[] = {'A', 0};

  uint32_t last_object_number = pdf_document->GetLastObjNum();
  EXPECT_FALSE(
      EPDFPage_AppendReusableUnicodeTextStampXObjectWithEmbeddedFontProbe(
          document(), page.get(), font_data.data(), font_data_size, nullptr,
          100, 40, 5, 10, 12, &placement, 1, 0, 0, 0, 1));
  EXPECT_EQ(last_object_number, pdf_document->GetLastObjNum());
  EXPECT_FALSE(
      EPDFPage_AppendReusableUnicodeTextStampXObjectWithStandardFontProbe(
          document(), page.get(), "Helvetica", nullptr, 100, 40, 5, 10, 12,
          &placement, 1, 0, 0, 0, 1));
  EXPECT_EQ(last_object_number, pdf_document->GetLastObjNum());

  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page.get());
  ASSERT_TRUE(pdf_page);
  pdf_page->GetMutableDict()->SetNewFor<CPDF_Number>(
      pdfium::page_object::kContents, 1);

  last_object_number = pdf_document->GetLastObjNum();
  EXPECT_FALSE(
      EPDFPage_AppendReusableUnicodeTextStampXObjectWithEmbeddedFontProbe(
          document(), page.get(), font_data.data(), font_data_size, kText, 100,
          40, 5, 10, 12, &placement, 1, 0, 0, 0, 1));
  EXPECT_EQ(last_object_number, pdf_document->GetLastObjNum());
  EXPECT_FALSE(
      EPDFPage_AppendReusableUnicodeTextStampXObjectWithStandardFontProbe(
          document(), page.get(), "Helvetica", kText, 100, 40, 5, 10, 12,
          &placement, 1, 0, 0, 0, 1));
  EXPECT_EQ(last_object_number, pdf_document->GetLastObjNum());
}

TEST_F(FPDFEditPageEmbedderTest, ReusableTextFormCanBeSharedAcrossPages) {
  CreateEmptyDocument();
  ScopedFPDFPage first(FPDFPage_New(document(), 0, 612, 792));
  ScopedFPDFPage second(FPDFPage_New(document(), 1, 612, 792));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ScopedFPDFFont font(FPDFText_LoadStandardFont(document(), "Helvetica"));
  ASSERT_TRUE(font);
  ASSERT_TRUE(CPDFFontFromFPDFFont(font.get())->IsStandardFont());
  EXPECT_EQ(nullptr, CPDFFontFromFPDFFont(font.get())->GetDocument());
  static constexpr FPDF_WCHAR kText[] = {'A', 0};

  CPDF_Document* pdf_document = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf_document);
  const uint32_t before_create = pdf_document->GetLastObjNum();
  const uint32_t form_object_number =
      EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
          document(), font.get(), kText, 100, 40, 5, 10, 12, 0, 0, 0, 0.5f);
  ASSERT_GT(form_object_number, 0u);
  EXPECT_GT(pdf_document->GetLastObjNum(), before_create);
  CPDF_Page* first_page = CPDFPageFromFPDFPage(first.get());
  CPDF_Page* second_page = CPDFPageFromFPDFPage(second.get());
  ASSERT_TRUE(first_page);
  ASSERT_TRUE(second_page);
  EXPECT_FALSE(first_page->GetDict()->GetObjectFor(
      pdfium::page_object::kContents));
  EXPECT_FALSE(second_page->GetDict()->GetObjectFor(
      pdfium::page_object::kContents));

  const FS_MATRIX first_placements[] = {{1, 0, 0, 1, 10, 10},
                                         {1, 0, 0, 1, 20, 20}};
  const FS_MATRIX second_placement = {1, 0, 0, 1, 30, 30};
  EXPECT_TRUE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), first.get(), form_object_number, first_placements, 2));
  EXPECT_TRUE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), second.get(), form_object_number, &second_placement, 1));

  RetainPtr<CPDF_Stream> form = ToStream(
      pdf_document->GetOrParseIndirectObject(form_object_number));
  ASSERT_TRUE(form);
  EXPECT_EQ("Form", form->GetDict()->GetNameFor("Subtype"));
  RetainPtr<const CPDF_Dictionary> form_resources =
      GetFormResources(pdf_document, form_object_number);
  ASSERT_TRUE(form_resources);
  ASSERT_TRUE(form_resources->GetDictFor("Font"));
  ASSERT_TRUE(form_resources->GetDictFor("ExtGState"));
  EXPECT_EQ(1u, form_resources->GetDictFor("Font")->size());
  EXPECT_EQ(1u, form_resources->GetDictFor("ExtGState")->size());
  EXPECT_TRUE(PageReferencesForm(first_page, form_object_number));
  EXPECT_TRUE(PageReferencesForm(second_page, form_object_number));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(first.get()));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(second.get()));
}

TEST_F(FPDFEditPageEmbedderTest,
       ReusableTextFormsRetainSharedEmbeddedFontAfterClose) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ASSERT_TRUE(page);
  const std::vector<uint8_t> font_data = GetFileContents(
      PathService::GetTestFilePath("fonts/ahem/Ahem.ttf").c_str());
  ASSERT_FALSE(font_data.empty());
  ScopedFPDFFont font(FPDFText_LoadFont(
      document(), font_data.data(),
      pdfium::checked_cast<uint32_t>(font_data.size()), FPDF_FONT_TRUETYPE,
      /*cid=*/true));
  ASSERT_TRUE(font);
  static constexpr FPDF_WCHAR kTexts[][2] = {{'A', 0}, {'B', 0}, {'C', 0}};
  CPDF_Document* pdf_document = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf_document);
  std::array<uint32_t, 3> forms;
  std::array<uint32_t, 3> font_objects;
  for (size_t i = 0; i < forms.size(); ++i) {
    forms[i] = EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
        document(), font.get(), kTexts[i], 100, 40, 5, 10, 12, 0, 0, 0, 1);
    ASSERT_GT(forms[i], 0u);
    font_objects[i] = GetOnlyFormFontObjectNumber(pdf_document, forms[i]);
    ASSERT_GT(font_objects[i], 0u);
    const FS_MATRIX placement = {1, 0, 0, 1, 10 + 40.0f * i, 10};
    ASSERT_TRUE(EPDFPage_AppendReusableFormXObjectProbe(
        document(), page.get(), forms[i], &placement, 1));
  }
  EXPECT_THAT(font_objects, Each(Eq(font_objects.front())));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(page.get()));
  font.reset();
  ASSERT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  ASSERT_TRUE(OpenSavedDocument());
  ScopedSavedPage saved_page = LoadScopedSavedPage(0);
  ASSERT_TRUE(saved_page);
  EXPECT_TRUE(RenderSavedPage(saved_page.get()));
  EXPECT_EQ(0, FPDFPage_GetAnnotCount(saved_page.get()));
  ScopedFPDFTextPage text_page(FPDFText_LoadPage(saved_page.get()));
  ASSERT_TRUE(text_page);
  const int character_count = FPDFText_CountChars(text_page.get());
  ASSERT_GT(character_count, 0);
  std::vector<unsigned int> extracted_text;
  for (int i = 0; i < character_count; ++i) {
    if (!FPDFText_IsGenerated(text_page.get(), i)) {
      extracted_text.push_back(FPDFText_GetUnicode(text_page.get(), i));
    }
  }
  EXPECT_THAT(extracted_text, ElementsAre('A', 'B', 'C'));
}

TEST_F(FPDFEditPageEmbedderTest,
       ReusableTextFormRejectsFontOwnedByDifferentDocument) {
  CreateEmptyDocument();
  ScopedFPDFDocument foreign_document(FPDF_CreateNewDocument());
  ASSERT_TRUE(foreign_document);
  const std::string font_path =
      PathService::GetTestFilePath("fonts/ahem/Ahem.ttf");
  const std::vector<uint8_t> font_data = GetFileContents(font_path.c_str());
  ASSERT_FALSE(font_data.empty());
  ScopedFPDFFont foreign_font(FPDFText_LoadFont(
      foreign_document.get(), font_data.data(),
      pdfium::checked_cast<uint32_t>(font_data.size()), FPDF_FONT_TRUETYPE,
      /*cid=*/true));
  ASSERT_TRUE(foreign_font);
  static constexpr FPDF_WCHAR kText[] = {'A', 0};
  CPDF_Document* pdf_document = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf_document);
  const uint32_t last_object_number = pdf_document->GetLastObjNum();

  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), foreign_font.get(), kText, 100, 40, 5, 10,
                    12, 0, 0, 0, 1));
  EXPECT_EQ(last_object_number, pdf_document->GetLastObjNum());
}

TEST_F(FPDFEditPageEmbedderTest, ReusableTextFormRejectsInvalidInputs) {
  CreateEmptyDocument();
  ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));
  ASSERT_TRUE(page);
  ScopedFPDFFont font(FPDFText_LoadStandardFont(document(), "Helvetica"));
  ASSERT_TRUE(font);
  static constexpr FPDF_WCHAR kText[] = {'A', 0};
  CPDF_Document* pdf_document = CPDFDocumentFromFPDFDocument(document());
  ASSERT_TRUE(pdf_document);
  const uint32_t last_object_number = pdf_document->GetLastObjNum();

  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), nullptr, kText, 100, 40, 5, 10, 12, 0, 0, 0,
                    1));
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), nullptr, 100, 40, 5, 10, 12, 0,
                    0, 0, 1));
  static constexpr FPDF_WCHAR kEmptyText[] = {0};
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), kEmptyText, 100, 40, 5, 10, 12,
                    0, 0, 0, 1));
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), kText, 0, 40, 5, 10, 12, 0, 0, 0,
                    1));
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), kText, 100, 40,
                    std::numeric_limits<float>::infinity(), 10, 12, 0, 0, 0,
                    1));
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), kText, 100, 40, 5, 10, 0, 0, 0,
                    0, 1));
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), kText, 100, 40, 5, 10, 12, 256,
                    0, 0, 1));
  EXPECT_EQ(0u, EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
                    document(), font.get(), kText, 100, 40, 5, 10, 12, 0, 0,
                    0, 1.1f));
  EXPECT_EQ(last_object_number, pdf_document->GetLastObjNum());

  CPDF_Page* pdf_page = CPDFPageFromFPDFPage(page.get());
  ASSERT_TRUE(pdf_page);
  EXPECT_FALSE(pdf_page->GetDict()->GetObjectFor(
      pdfium::page_object::kContents));
  ASSERT_TRUE(pdf_page->GetResources());
  EXPECT_EQ(0u, pdf_page->GetResources()->size());
  const FS_MATRIX placement = {1, 0, 0, 1, 10, 10};
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), page.get(), 0, &placement, 1));
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), page.get(), last_object_number + 100, &placement, 1));

  const uint32_t form_object_number =
      EPDFTextObj_CreateReusableUnicodeTextFormXObjectProbe(
          document(), font.get(), kText, 100, 40, 5, 10, 12, 0, 0, 0, 1);
  ASSERT_GT(form_object_number, 0u);
  ScopedFPDFDocument foreign_document(FPDF_CreateNewDocument());
  ASSERT_TRUE(foreign_document);
  ScopedFPDFPage foreign_page(
      FPDFPage_New(foreign_document.get(), 0, 612, 792));
  ASSERT_TRUE(foreign_page);
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), foreign_page.get(), form_object_number, &placement, 1));
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      foreign_document.get(), page.get(), form_object_number, &placement, 1));

  const FS_MATRIX invalid_placement = {1, 0, 0, 1,
                                       std::numeric_limits<float>::infinity(),
                                       10};
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), page.get(), form_object_number, &invalid_placement, 1));
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), page.get(), form_object_number, nullptr, 1));
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), page.get(), form_object_number, &placement, 0));
  EXPECT_FALSE(pdf_page->GetDict()->GetObjectFor(
      pdfium::page_object::kContents));
  ASSERT_TRUE(pdf_page->GetResources());
  EXPECT_EQ(0u, pdf_page->GetResources()->size());

  RetainPtr<CPDF_Dictionary> non_form =
      pdf_document->NewIndirect<CPDF_Dictionary>();
  ASSERT_TRUE(non_form);
  EXPECT_FALSE(EPDFPage_AppendReusableFormXObjectProbe(
      document(), page.get(), non_form->GetObjNum(), &placement, 1));
  EXPECT_FALSE(pdf_page->GetDict()->GetObjectFor(
      pdfium::page_object::kContents));
  EXPECT_EQ(0u, pdf_page->GetResources()->size());
}

TEST_F(FPDFEditPageEmbedderTest, Rotation) {
  const char* rotated_checksum = []() {
    if (CFX_DefaultRenderDevice::UseSkiaRenderer()) {
      return "eded83f75f3d0332c584c416c571c0df";
    }
    return "d599429574ff0dcad3bc898ea8b874ca";
  }();

  {
    ASSERT_TRUE(OpenDocument("rectangles.pdf"));
    ScopedPage page = LoadScopedPage(0);
    ASSERT_TRUE(page);

    {
      // Render the page as is.
      EXPECT_EQ(0, FPDFPage_GetRotation(page.get()));
      const int page_width = static_cast<int>(FPDF_GetPageWidth(page.get()));
      const int page_height = static_cast<int>(FPDF_GetPageHeight(page.get()));
      EXPECT_EQ(200, page_width);
      EXPECT_EQ(300, page_height);
      ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
      CompareBitmap(bitmap.get(), page_width, page_height,
                    pdfium::RectanglesChecksum());
    }

    FPDFPage_SetRotation(page.get(), 1);

    {
      // Render the page after rotation.
      // Note that the change affects the rendering, as expected.
      // It behaves just like the case below, rather than the case above.
      EXPECT_EQ(1, FPDFPage_GetRotation(page.get()));
      const int page_width = static_cast<int>(FPDF_GetPageWidth(page.get()));
      const int page_height = static_cast<int>(FPDF_GetPageHeight(page.get()));
      EXPECT_EQ(300, page_width);
      EXPECT_EQ(200, page_height);
      ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
      CompareBitmap(bitmap.get(), page_width, page_height, rotated_checksum);
    }
  }

  {
    // Save a copy, open the copy, and render it.
    // Note that it renders the rotation.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    EXPECT_EQ(1, FPDFPage_GetRotation(saved_page));
    const int page_width = static_cast<int>(FPDF_GetPageWidth(saved_page));
    const int page_height = static_cast<int>(FPDF_GetPageHeight(saved_page));
    EXPECT_EQ(300, page_width);
    EXPECT_EQ(200, page_height);
    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height, rotated_checksum);

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }
}

TEST_F(FPDFEditPageEmbedderTest, HasTransparencyImage) {
  static constexpr int kExpectedObjectCount = 39;
  ASSERT_TRUE(OpenDocument("embedded_images.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(kExpectedObjectCount, FPDFPage_CountObjects(page.get()));

  for (int i = 0; i < kExpectedObjectCount; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), i);
    EXPECT_FALSE(FPDFPageObj_HasTransparency(obj));

    FPDFPageObj_SetFillColor(obj, 255, 0, 0, 127);
    EXPECT_TRUE(FPDFPageObj_HasTransparency(obj));
  }
}

TEST_F(FPDFEditPageEmbedderTest, HasTransparencyInvalid) {
  EXPECT_FALSE(FPDFPageObj_HasTransparency(nullptr));
}

TEST_F(FPDFEditPageEmbedderTest, HasTransparencyPath) {
  static constexpr int kExpectedObjectCount = 8;
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(kExpectedObjectCount, FPDFPage_CountObjects(page.get()));

  for (int i = 0; i < kExpectedObjectCount; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), i);
    EXPECT_FALSE(FPDFPageObj_HasTransparency(obj));

    FPDFPageObj_SetStrokeColor(obj, 63, 63, 0, 127);
    EXPECT_TRUE(FPDFPageObj_HasTransparency(obj));
  }
}

TEST_F(FPDFEditPageEmbedderTest, HasTransparencyText) {
  static constexpr int kExpectedObjectCount = 2;
  ASSERT_TRUE(OpenDocument("text_render_mode.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  ASSERT_EQ(kExpectedObjectCount, FPDFPage_CountObjects(page.get()));

  for (int i = 0; i < kExpectedObjectCount; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), i);
    EXPECT_FALSE(FPDFPageObj_HasTransparency(obj));

    FPDFPageObj_SetBlendMode(obj, "Lighten");
    EXPECT_TRUE(FPDFPageObj_HasTransparency(obj));
  }
}

TEST_F(FPDFEditPageEmbedderTest, GetFillAndStrokeForImage) {
  static constexpr int kExpectedObjectCount = 39;
  static constexpr int kImageObjectsStartIndex = 33;
  ASSERT_TRUE(OpenDocument("embedded_images.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_EQ(kExpectedObjectCount, FPDFPage_CountObjects(page.get()));

  for (int i = kImageObjectsStartIndex; i < kExpectedObjectCount; ++i) {
    FPDF_PAGEOBJECT image = FPDFPage_GetObject(page.get(), i);
    ASSERT_TRUE(image);
    EXPECT_EQ(FPDF_PAGEOBJ_IMAGE, FPDFPageObj_GetType(image));

    unsigned int r;
    unsigned int g;
    unsigned int b;
    unsigned int a;
    EXPECT_FALSE(FPDFPageObj_GetFillColor(image, &r, &g, &b, &a));
    EXPECT_FALSE(FPDFPageObj_GetStrokeColor(image, &r, &g, &b, &a));
  }
}

TEST_F(FPDFEditPageEmbedderTest, DashingArrayAndPhase) {
  {
    EXPECT_FALSE(FPDFPageObj_GetDashPhase(nullptr, nullptr));

    float phase = -1123.5f;
    EXPECT_FALSE(FPDFPageObj_GetDashPhase(nullptr, &phase));
    EXPECT_FLOAT_EQ(-1123.5f, phase);

    EXPECT_EQ(-1, FPDFPageObj_GetDashCount(nullptr));

    EXPECT_FALSE(FPDFPageObj_GetDashArray(nullptr, nullptr, 3));

    std::array<float, 3> get_array = {{-1.0f, -1.0f, -1.0f}};
    EXPECT_FALSE(
        FPDFPageObj_GetDashArray(nullptr, get_array.data(), get_array.size()));
    EXPECT_THAT(get_array, Each(FloatEq(-1.0f)));

    EXPECT_FALSE(FPDFPageObj_SetDashPhase(nullptr, 5.0f));
    EXPECT_FALSE(FPDFPageObj_SetDashArray(nullptr, nullptr, 3, 5.0f));

    std::array<float, 3> set_array = {{1.0f, 2.0f, 3.0f}};
    EXPECT_FALSE(FPDFPageObj_SetDashArray(nullptr, set_array.data(),
                                          set_array.size(), 5.0f));
  }

  static constexpr int kExpectedObjectCount = 3;
  ASSERT_TRUE(OpenDocument("dashed_lines.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  ASSERT_EQ(kExpectedObjectCount, FPDFPage_CountObjects(page.get()));

  {
    FPDF_PAGEOBJECT path = FPDFPage_GetObject(page.get(), 0);
    ASSERT_TRUE(path);
    EXPECT_EQ(FPDF_PAGEOBJ_PATH, FPDFPageObj_GetType(path));

    EXPECT_FALSE(FPDFPageObj_GetDashPhase(path, nullptr));
    EXPECT_FALSE(FPDFPageObj_GetDashArray(path, nullptr, 3));
    EXPECT_FALSE(FPDFPageObj_SetDashArray(path, nullptr, 3, 5.0f));

    float phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_FLOAT_EQ(0.0f, phase);
    EXPECT_EQ(0, FPDFPageObj_GetDashCount(path));

    std::array<float, 3> get_array = {{-1.0f, -1.0f, -1.0f}};
    EXPECT_TRUE(
        FPDFPageObj_GetDashArray(path, get_array.data(), get_array.size()));
    EXPECT_THAT(get_array, Each(FloatEq(-1.0f)));
  }

  {
    FPDF_PAGEOBJECT path = FPDFPage_GetObject(page.get(), 1);
    ASSERT_TRUE(path);
    EXPECT_EQ(FPDF_PAGEOBJ_PATH, FPDFPageObj_GetType(path));

    float phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_LT(0.0f, phase);
    ASSERT_EQ(6, FPDFPageObj_GetDashCount(path));

    std::array<float, 6> dash_array = {
        {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f}};
    ASSERT_TRUE(
        FPDFPageObj_GetDashArray(path, dash_array.data(), dash_array.size()));
    EXPECT_THAT(dash_array, Each(Gt(0.0f)));

    // the array is decreasing in value.
    for (int i = 0; i < 5; i++) {
      EXPECT_GT(dash_array[i], dash_array[i + 1]);
    }
    // modify phase
    EXPECT_TRUE(FPDFPageObj_SetDashPhase(path, 1.0f));

    phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_FLOAT_EQ(1.0f, phase);

    // clear array
    EXPECT_TRUE(FPDFPageObj_SetDashArray(path, nullptr, 0, 0.0f));
    EXPECT_EQ(0, FPDFPageObj_GetDashCount(path));

    phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_FLOAT_EQ(0.0f, phase);
  }

  {
    FPDF_PAGEOBJECT path = FPDFPage_GetObject(page.get(), 2);
    ASSERT_TRUE(path);
    EXPECT_EQ(FPDF_PAGEOBJ_PATH, FPDFPageObj_GetType(path));

    float phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_FLOAT_EQ(0.0f, phase);

    EXPECT_EQ(0, FPDFPageObj_GetDashCount(path));

    // `get_array` should be unmodified
    std::array<float, 4> get_array = {{-1.0f, -1.0f, -1.0f, -1.0f}};
    EXPECT_TRUE(
        FPDFPageObj_GetDashArray(path, get_array.data(), get_array.size()));
    EXPECT_THAT(get_array, Each(FloatEq(-1.0f)));

    // modify dash_array and phase
    const std::array<float, 3> set_array = {{1.0f, 2.0f, 3.0f}};
    EXPECT_TRUE(FPDFPageObj_SetDashArray(path, set_array.data(),
                                         set_array.size(), 5.0f));

    phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_FLOAT_EQ(5.0f, phase);
    ASSERT_EQ(3, FPDFPageObj_GetDashCount(path));

    // Pretend `get_array` has too few members.
    EXPECT_FALSE(FPDFPageObj_GetDashArray(path, get_array.data(), 2));
    EXPECT_THAT(get_array, Each(FloatEq(-1.0f)));

    ASSERT_TRUE(
        FPDFPageObj_GetDashArray(path, get_array.data(), get_array.size()));

    // `get_array` should be modified only up to dash_count
    for (int i = 0; i < 3; i++) {
      EXPECT_FLOAT_EQ(static_cast<float>(i + 1), get_array[i]);
    }
    EXPECT_FLOAT_EQ(-1.0f, get_array[3]);

    // clear array
    EXPECT_TRUE(FPDFPageObj_SetDashArray(path, set_array.data(), 0, 4.0f));
    EXPECT_EQ(0, FPDFPageObj_GetDashCount(path));

    phase = -1123.5f;
    EXPECT_TRUE(FPDFPageObj_GetDashPhase(path, &phase));
    EXPECT_FLOAT_EQ(4.0f, phase);
  }
}

TEST_F(FPDFEditPageEmbedderTest, GetRotatedBoundsBadParameters) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), 0);
  ASSERT_EQ(FPDF_PAGEOBJ_TEXT, FPDFPageObj_GetType(obj));

  FS_QUADPOINTSF quad;
  ASSERT_FALSE(FPDFPageObj_GetRotatedBounds(nullptr, nullptr));
  ASSERT_FALSE(FPDFPageObj_GetRotatedBounds(obj, nullptr));
  ASSERT_FALSE(FPDFPageObj_GetRotatedBounds(nullptr, &quad));
}

TEST_F(FPDFEditPageEmbedderTest, GetBoundsForNormalText) {
  ASSERT_TRUE(OpenDocument("hello_world.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), 0);
  ASSERT_EQ(FPDF_PAGEOBJ_TEXT, FPDFPageObj_GetType(obj));

  static constexpr float kExpectedLeft = 20.348f;
  static constexpr float kExpectedBottom = 48.164f;
  static constexpr float kExpectedRight = 83.36f;
  static constexpr float kExpectedTop = 58.328f;

  float left;
  float bottom;
  float right;
  float top;
  ASSERT_TRUE(FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top));
  EXPECT_FLOAT_EQ(kExpectedLeft, left);
  EXPECT_FLOAT_EQ(kExpectedBottom, bottom);
  EXPECT_FLOAT_EQ(kExpectedRight, right);
  EXPECT_FLOAT_EQ(kExpectedTop, top);

  FS_QUADPOINTSF quad;
  ASSERT_TRUE(FPDFPageObj_GetRotatedBounds(obj, &quad));
  EXPECT_FLOAT_EQ(kExpectedLeft, quad.x1);
  EXPECT_FLOAT_EQ(kExpectedBottom, quad.y1);
  EXPECT_FLOAT_EQ(kExpectedRight, quad.x2);
  EXPECT_FLOAT_EQ(kExpectedBottom, quad.y2);
  EXPECT_FLOAT_EQ(kExpectedRight, quad.x3);
  EXPECT_FLOAT_EQ(kExpectedTop, quad.y3);
  EXPECT_FLOAT_EQ(kExpectedLeft, quad.x4);
  EXPECT_FLOAT_EQ(kExpectedTop, quad.y4);
}

TEST_F(FPDFEditPageEmbedderTest, GetBoundsForRotatedText) {
  ASSERT_TRUE(OpenDocument("rotated_text.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), 0);
  ASSERT_EQ(FPDF_PAGEOBJ_TEXT, FPDFPageObj_GetType(obj));

  static constexpr float kExpectedLeft = 98.9478f;
  static constexpr float kExpectedBottom = 78.2607f;
  static constexpr float kExpectedRight = 126.32983f;
  static constexpr float kExpectedTop = 105.64272f;

  float left;
  float bottom;
  float right;
  float top;
  ASSERT_TRUE(FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top));
  EXPECT_FLOAT_EQ(kExpectedLeft, left);
  EXPECT_FLOAT_EQ(kExpectedBottom, bottom);
  EXPECT_FLOAT_EQ(kExpectedRight, right);
  EXPECT_FLOAT_EQ(kExpectedTop, top);

  FS_QUADPOINTSF quad;
  ASSERT_TRUE(FPDFPageObj_GetRotatedBounds(obj, &quad));
  EXPECT_FLOAT_EQ(kExpectedLeft, quad.x1);
  EXPECT_FLOAT_EQ(98.4557f, quad.y1);
  EXPECT_FLOAT_EQ(119.14279f, quad.x2);
  EXPECT_FLOAT_EQ(kExpectedBottom, quad.y2);
  EXPECT_FLOAT_EQ(kExpectedRight, quad.x3);
  EXPECT_FLOAT_EQ(85.447739f, quad.y3);
  EXPECT_FLOAT_EQ(106.13486f, quad.x4);
  EXPECT_FLOAT_EQ(kExpectedTop, quad.y4);
}

TEST_F(FPDFEditPageEmbedderTest, GetBoundsForNormalImage) {
  ASSERT_TRUE(OpenDocument("matte.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), 2);
  ASSERT_EQ(FPDF_PAGEOBJ_IMAGE, FPDFPageObj_GetType(obj));

  static constexpr float kExpectedLeft = 0.0f;
  static constexpr float kExpectedBottom = 90.0f;
  static constexpr float kExpectedRight = 40.0f;
  static constexpr float kExpectedTop = 150.0f;

  float left;
  float bottom;
  float right;
  float top;
  ASSERT_TRUE(FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top));
  EXPECT_FLOAT_EQ(kExpectedLeft, left);
  EXPECT_FLOAT_EQ(kExpectedBottom, bottom);
  EXPECT_FLOAT_EQ(kExpectedRight, right);
  EXPECT_FLOAT_EQ(kExpectedTop, top);

  FS_QUADPOINTSF quad;
  ASSERT_TRUE(FPDFPageObj_GetRotatedBounds(obj, &quad));
  EXPECT_FLOAT_EQ(kExpectedLeft, quad.x1);
  EXPECT_FLOAT_EQ(kExpectedBottom, quad.y1);
  EXPECT_FLOAT_EQ(kExpectedRight, quad.x2);
  EXPECT_FLOAT_EQ(kExpectedBottom, quad.y2);
  EXPECT_FLOAT_EQ(kExpectedRight, quad.x3);
  EXPECT_FLOAT_EQ(kExpectedTop, quad.y3);
  EXPECT_FLOAT_EQ(kExpectedLeft, quad.x4);
  EXPECT_FLOAT_EQ(kExpectedTop, quad.y4);
}

TEST_F(FPDFEditPageEmbedderTest, GetBoundsForRotatedImage) {
  ASSERT_TRUE(OpenDocument("rotated_image.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);

  FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.get(), 0);
  ASSERT_EQ(FPDF_PAGEOBJ_IMAGE, FPDFPageObj_GetType(obj));

  static constexpr float kExpectedLeft = 100.0f;
  static constexpr float kExpectedBottom = 70.0f;
  static constexpr float kExpectedRight = 170.0f;
  static constexpr float kExpectedTop = 140.0f;

  float left;
  float bottom;
  float right;
  float top;
  ASSERT_TRUE(FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top));
  EXPECT_FLOAT_EQ(kExpectedLeft, left);
  EXPECT_FLOAT_EQ(kExpectedBottom, bottom);
  EXPECT_FLOAT_EQ(kExpectedRight, right);
  EXPECT_FLOAT_EQ(kExpectedTop, top);

  FS_QUADPOINTSF quad;
  ASSERT_TRUE(FPDFPageObj_GetRotatedBounds(obj, &quad));
  EXPECT_FLOAT_EQ(kExpectedLeft, quad.x1);
  EXPECT_FLOAT_EQ(100.0f, quad.y1);
  EXPECT_FLOAT_EQ(130.0f, quad.x2);
  EXPECT_FLOAT_EQ(kExpectedBottom, quad.y2);
  EXPECT_FLOAT_EQ(kExpectedRight, quad.x3);
  EXPECT_FLOAT_EQ(110.0f, quad.y3);
  EXPECT_FLOAT_EQ(140.0f, quad.x4);
  EXPECT_FLOAT_EQ(kExpectedTop, quad.y4);
}

TEST_F(FPDFEditPageEmbedderTest, VerifyDashArraySaved) {
  static constexpr float kDashArray[] = {2.5, 3.6};
  static constexpr float kDashPhase = 1.2;

  CreateEmptyDocument();
  {
    ScopedFPDFPage page(FPDFPage_New(document(), 0, 612, 792));

    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(400, 100);
    EXPECT_TRUE(FPDFPageObj_SetStrokeWidth(path, 2));
    EXPECT_TRUE(FPDFPageObj_SetStrokeColor(path, 255, 0, 0, 255));
    EXPECT_TRUE(FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, 1));
    EXPECT_TRUE(FPDFPath_LineTo(path, 200, 200));
    EXPECT_TRUE(FPDFPageObj_SetDashArray(path, kDashArray,
                                         std::size(kDashArray), kDashPhase));
    FPDFPage_InsertObject(page.get(), path);

    EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
    path = FPDFPage_GetObject(page.get(), 0);
    ASSERT_TRUE(path);
    ASSERT_EQ(2, FPDFPageObj_GetDashCount(path));

    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
  }

  ASSERT_TRUE(OpenSavedDocument());
  FPDF_PAGE page = LoadSavedPage(0);
  ASSERT_TRUE(page);

  FPDF_PAGEOBJECT path = FPDFPage_GetObject(page, 0);
  ASSERT_TRUE(path);

  float dash_array[] = {0, 0};
  ASSERT_EQ(static_cast<int>(std::size(dash_array)),
            FPDFPageObj_GetDashCount(path));
  ASSERT_TRUE(
      FPDFPageObj_GetDashArray(path, dash_array, std::size(dash_array)));
  ASSERT_EQ(kDashArray[0], dash_array[0]);
  ASSERT_EQ(kDashArray[1], dash_array[1]);
  float dash_phase = 0;
  ASSERT_TRUE(FPDFPageObj_GetDashPhase(path, &dash_phase));
  ASSERT_EQ(kDashPhase, dash_phase);

  CloseSavedPage(page);
  CloseSavedDocument();
}

TEST_F(FPDFEditPageEmbedderTest, PageObjectActiveState) {
  const char* one_rectangle_inactive_checksum = []() {
    if (CFX_DefaultRenderDevice::UseSkiaRenderer()) {
      return "cf5bb4e61609162c03f4c8a6d9791230";
    }
    return "0481e8936b35ac9484b51a0966ab4ab6";
  }();

  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const int page_width = static_cast<int>(FPDF_GetPageWidth(page.get()));
  const int page_height = static_cast<int>(FPDF_GetPageHeight(page.get()));

  // Note the original count of page objects for the rectangles.
  EXPECT_EQ(8, FPDFPage_CountObjects(page.get()));

  {
    // Render the page as is.
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height,
                  pdfium::RectanglesChecksum());
  }

  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    // Note that all page objects for the rectangles are present in the copy.
    EXPECT_EQ(8, FPDFPage_CountObjects(saved_page));

    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height,
                  pdfium::RectanglesChecksum());

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }

  // Mark one of the page objects as inactive.  It is still present in the page.
  FPDF_PAGEOBJECT page_obj = FPDFPage_GetObject(page.get(), 4);
  ASSERT_TRUE(page_obj);

  // Negative testing.
  EXPECT_FALSE(FPDFPageObj_GetIsActive(page_obj, nullptr));
  FPDF_BOOL page_obj_is_active;
  EXPECT_FALSE(FPDFPageObj_GetIsActive(nullptr, &page_obj_is_active));
  EXPECT_FALSE(FPDFPageObj_GetIsActive(nullptr, nullptr));

  // Positive testing.
  page_obj_is_active = false;
  EXPECT_TRUE(FPDFPageObj_GetIsActive(page_obj, &page_obj_is_active));
  EXPECT_TRUE(page_obj_is_active);
  ASSERT_TRUE(FPDFPageObj_SetIsActive(page_obj, /*active=*/false));
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
  EXPECT_EQ(8, FPDFPage_CountObjects(page.get()));
  EXPECT_TRUE(FPDFPageObj_GetIsActive(page_obj, &page_obj_is_active));
  EXPECT_FALSE(page_obj_is_active);

  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    // Note that a rectangle is absent from the copy.
    EXPECT_EQ(7, FPDFPage_CountObjects(saved_page));

    // The absence of the inactive page object affects the rendered result.
    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height,
                  one_rectangle_inactive_checksum);

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }

  // Negative testing.
  EXPECT_FALSE(FPDFPageObj_SetIsActive(nullptr, false));
  EXPECT_FALSE(FPDFPageObj_SetIsActive(nullptr, true));
}

TEST_F(FPDFEditPageEmbedderTest, Bug378120423) {
  const char kChecksum[] = "b53fb03e2bc41ef18d4ba61f0f681365";
  const char kBlankChecksum[] = "eee4600ac08b458ac7ac2320e225674c";

  ASSERT_TRUE(OpenDocument("bug_378120423.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const int page_width = static_cast<int>(FPDF_GetPageWidth(page.get()));
  const int page_height = static_cast<int>(FPDF_GetPageHeight(page.get()));

  {
    // Render the page as is.
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height, kChecksum);
    EXPECT_EQ(1, FPDFPage_CountObjects(page.get()));
  }

  // Deactivate `page_obj` and render.
  FPDF_PAGEOBJECT page_obj = FPDFPage_GetObject(page.get(), 0);
  ASSERT_TRUE(FPDFPageObj_SetIsActive(page_obj, false));
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height, kBlankChecksum);
    // `page_obj` can still be found. It is just deactivated.
    EXPECT_EQ(1, FPDFPage_CountObjects(page.get()));
  }

  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height, kBlankChecksum);
    // `page_obj` did not get written out to the saved PDF.
    EXPECT_EQ(0, FPDFPage_CountObjects(saved_page));

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }

  // Reactivate `page_obj` and render.
  ASSERT_TRUE(FPDFPageObj_SetIsActive(page_obj, true));
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height, kChecksum);
    EXPECT_EQ(1, FPDFPage_CountObjects(page.get()));
  }

  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height, kChecksum);
    EXPECT_EQ(1, FPDFPage_CountObjects(saved_page));

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }
}

TEST_F(FPDFEditPageEmbedderTest, Bug378464305) {
  ASSERT_TRUE(OpenDocument("rectangles.pdf"));
  ScopedPage page = LoadScopedPage(0);
  ASSERT_TRUE(page);
  const int page_width = static_cast<int>(FPDF_GetPageWidth(page.get()));
  const int page_height = static_cast<int>(FPDF_GetPageHeight(page.get()));
  static constexpr int kOriginalObjectCount = 8;
  {
    // Sanity check rectangles.pdf before modifying it.
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height,
                  pdfium::RectanglesChecksum());
    EXPECT_EQ(kOriginalObjectCount, FPDFPage_CountObjects(page.get()));
  }

  // Add a new path.
  static constexpr int kObjectCountWithNewPath = kOriginalObjectCount + 1;
  ScopedFPDFPageObject path_wrapper(FPDFPageObj_CreateNewPath(50, 50));
  FPDF_PAGEOBJECT path = path_wrapper.get();
  ASSERT_TRUE(path);
  EXPECT_TRUE(
      FPDFPath_SetDrawMode(path, FPDF_FILLMODE_WINDING, /*stroke=*/false));
  EXPECT_TRUE(
      FPDFPageObj_SetFillColor(path, /*R=*/255, /*G=*/0, /*B=*/0, /*A=*/127));
  EXPECT_TRUE(FPDFPath_LineTo(path, 40, 60));
  EXPECT_TRUE(FPDFPath_LineTo(path, 40, 50));
  EXPECT_TRUE(FPDFPath_Close(path));
  FPDFPage_InsertObject(page.get(), path_wrapper.release());
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));

  // Render `page` with the new path.
  const char* new_path_checksum = []() {
    if (CFX_DefaultRenderDevice::UseSkiaRenderer()) {
      return "34b57c038e2927ac490c20dc2c7fb706";
    }
    return "725702098ecb591a356827d54bd26cb2";
  }();
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height, new_path_checksum);
    EXPECT_EQ(kObjectCountWithNewPath, FPDFPage_CountObjects(page.get()));
  }
  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height, new_path_checksum);
    EXPECT_EQ(kObjectCountWithNewPath, FPDFPage_CountObjects(saved_page));

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }

  // Deactivate `path` and render.
  ASSERT_TRUE(FPDFPageObj_SetIsActive(path, false));
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height,
                  pdfium::RectanglesChecksum());
    // `path` can still be found. It is just deactivated.
    EXPECT_EQ(kObjectCountWithNewPath, FPDFPage_CountObjects(page.get()));
  }

  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height,
                  pdfium::RectanglesChecksum());
    // `path` did not get written out to the saved PDF.
    EXPECT_EQ(kOriginalObjectCount, FPDFPage_CountObjects(saved_page));

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }

  // Reactivate `path` and render.
  ASSERT_TRUE(FPDFPageObj_SetIsActive(path, true));
  EXPECT_TRUE(FPDFPage_GenerateContent(page.get()));
  {
    ScopedFPDFBitmap bitmap = RenderLoadedPage(page.get());
    CompareBitmap(bitmap.get(), page_width, page_height, new_path_checksum);
    EXPECT_EQ(kObjectCountWithNewPath, FPDFPage_CountObjects(page.get()));
  }

  {
    // Save a copy, open the copy, and render it.
    EXPECT_TRUE(FPDF_SaveAsCopy(document(), this, 0));
    ASSERT_TRUE(OpenSavedDocument());
    FPDF_PAGE saved_page = LoadSavedPage(0);
    ASSERT_TRUE(saved_page);

    ScopedFPDFBitmap bitmap = RenderSavedPage(saved_page);
    CompareBitmap(bitmap.get(), page_width, page_height, new_path_checksum);
    EXPECT_EQ(kObjectCountWithNewPath, FPDFPage_CountObjects(saved_page));

    CloseSavedPage(saved_page);
    CloseSavedDocument();
  }
}
