// Copyright 2024 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "public/fpdf_save_chunked.h"

#include <memory>
#include <optional>

#include "core/fpdfapi/edit/cpdf_creator.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "fpdfsdk/cpdfsdk_filewriteadapter.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// Session data for chunked PDF save
struct FPDF_SaveSessionData {
  std::unique_ptr<CPDF_Creator> creator;
  bool is_complete = false;
  bool has_error = false;
};

}  // namespace

FPDF_EXPORT FPDF_SAVE_SESSION FPDF_CALLCONV
FPDF_CreateSaveSession(FPDF_DOCUMENT document,
                       FPDF_FILEWRITE* pFileWrite,
                       FPDF_DWORD flags,
                       int fileVersion) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc || !pFileWrite) {
    return nullptr;
  }

  auto session = std::make_unique<FPDF_SaveSessionData>();
  
  // Create CPDF_Creator with file write adapter
  session->creator = std::make_unique<CPDF_Creator>(
      pDoc, pdfium::MakeRetain<CPDFSDK_FileWriteAdapter>(pFileWrite));

  // Set file version if specified
  if (fileVersion >= 10 && fileVersion <= 17) {
    if (!session->creator->SetFileVersion(fileVersion)) {
      return nullptr;
    }
  }

  // Set security removal flag if requested
  if (flags == FPDF_REMOVE_SECURITY) {
    session->creator->RemoveSecurity();
    flags = 0;  // Reset flags after processing
  }

  // Initialize the save process (but don't execute Continue())
  // This separates initialization from execution for chunked saving
  if (!session->creator->Initialize(flags)) {
    return nullptr;
  }

  return reinterpret_cast<FPDF_SAVE_SESSION>(session.release());
}

FPDF_EXPORT int FPDF_CALLCONV
FPDF_SaveNextChunk(FPDF_SAVE_SESSION session) {
  auto* data = reinterpret_cast<FPDF_SaveSessionData*>(session);
  if (!data) {
    return -1;  // Invalid session
  }

  if (data->is_complete) {
    return 0;  // Already complete
  }

  if (data->has_error) {
    return -1;  // Previous error
  }

  // Execute one incremental step
  // WriteOldObjs/WriteNewObjs now yield internally every ~16KB,
  // so we don't need an outer loop to accumulate chunks
  int result = data->creator->ContinueOneStep();

  if (result == 0) {
    // Generation complete
    data->is_complete = true;
  } else if (result < 0) {
    // Error occurred
    data->has_error = true;
  }

  return result;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDF_GetSaveProgress(FPDF_SAVE_SESSION session) {
  auto* data = reinterpret_cast<FPDF_SaveSessionData*>(session);
  if (!data) {
    return -1;
  }

  if (data->has_error) {
    return -1;
  }

  if (data->is_complete) {
    return 100;
  }

  // Return the actual stage-based progress (0-100)
  return data->creator->GetProgress();
}

FPDF_EXPORT void FPDF_CALLCONV
FPDF_DestroySaveSession(FPDF_SAVE_SESSION session) {
  delete reinterpret_cast<FPDF_SaveSessionData*>(session);
}
