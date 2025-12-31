// Copyright 2024 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Chunked PDF save API for asynchronous/streaming PDF generation.
// Allows incremental generation of PDF data to prevent memory spikes.

#ifndef PUBLIC_FPDF_SAVE_CHUNKED_H_
#define PUBLIC_FPDF_SAVE_CHUNKED_H_

// NOLINTNEXTLINE(build/include)
#include "fpdf_save.h"
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a chunked save session.
typedef struct FPDF_SAVE_SESSION_* FPDF_SAVE_SESSION;

// Function: FPDF_CreateSaveSession
//          Creates a new chunked save session for asynchronous PDF generation.
//          This allows PDF data to be generated incrementally to avoid memory
//          spikes with large documents.
// Parameters:
//          document      -   Handle to document to save.
//          pFileWrite    -   Pointer to custom file write structure.
//          flags         -   Save flags (FPDF_INCREMENTAL, etc).
//          fileVersion   -   PDF version (14 for 1.4, 15 for 1.5, etc).
//                           Pass 0 to use default version.
// Return value:
//          Session handle on success, NULL on failure.
// Comments:
//          Must call FPDF_DestroySaveSession when done to release resources.
//
FPDF_EXPORT FPDF_SAVE_SESSION FPDF_CALLCONV
FPDF_CreateSaveSession(FPDF_DOCUMENT document,
                       FPDF_FILEWRITE* pFileWrite,
                       FPDF_DWORD flags,
                       int fileVersion);

// Function: FPDF_SaveNextChunk
//          Generates the next chunk of PDF data (approximately 64KB).
//          Call repeatedly until it returns 0 to generate the complete PDF.
// Parameters:
//          session       -   Save session handle from FPDF_CreateSaveSession.
// Return value:
//          1 if more data remains to be generated.
//          0 if generation is complete.
//         -1 on error.
// Comments:
//          Data is written via the WriteBlock callback provided to
//          FPDF_CreateSaveSession. This function may be called from async
//          contexts (e.g., JavaScript setTimeout) to avoid blocking.
//
FPDF_EXPORT int FPDF_CALLCONV
FPDF_SaveNextChunk(FPDF_SAVE_SESSION session);

// Function: FPDF_GetSaveProgress
//          Gets the current progress of the save operation (0-100).
// Parameters:
//          session       -   Save session handle.
// Return value:
//          Progress percentage (0-100), or -1 if session is invalid.
//
FPDF_EXPORT int FPDF_CALLCONV
FPDF_GetSaveProgress(FPDF_SAVE_SESSION session);

// Function: FPDF_DestroySaveSession
//          Destroys a save session and releases all associated resources.
// Parameters:
//          session       -   Save session handle.
// Comments:
//          It is safe to call this even if generation is incomplete.
//          Any unwritten data will be discarded.
//
FPDF_EXPORT void FPDF_CALLCONV
FPDF_DestroySaveSession(FPDF_SAVE_SESSION session);

#ifdef __cplusplus
}
#endif

#endif  // PUBLIC_FPDF_SAVE_CHUNKED_H_
