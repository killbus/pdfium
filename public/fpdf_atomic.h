#ifndef PUBLIC_FPDF_ATOMIC_H_
#define PUBLIC_FPDF_ATOMIC_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Atomic Object Model (AOM) Extension
// Provides direct access to PDFium's internal objects for structural editing.
// ----------------------------------------------------------------------------

typedef void* FPDF_OBJECT;
typedef int   FPDF_BOOL;

// ----------------------------------------------------------------------------
// 1. Document Level Access
// ----------------------------------------------------------------------------

// Get the Root Dictionary (Catalog) of the document.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_GetRoot(FPDF_DOCUMENT document);

// Get the Trailer Dictionary.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_GetTrailer(FPDF_DOCUMENT document);

// Get the Info Dictionary.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_GetInfoDict(FPDF_DOCUMENT document);


// ----------------------------------------------------------------------------
// 2. Object Creation & Indirect Objects
// ----------------------------------------------------------------------------

// Creates a new indirect Dictionary (e.g., "10 0 R"). Returns direct pointer to it.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewIndirectDict(FPDF_DOCUMENT document);

// Creates a new indirect Array.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewIndirectArray(FPDF_DOCUMENT document);

// Creates a new direct Dictionary.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewDict(FPDF_DOCUMENT document);

// Creates a new direct Array.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewArray(FPDF_DOCUMENT document);


// ----------------------------------------------------------------------------
// 3. Scalar Creation
// ----------------------------------------------------------------------------

// Create a String (Text) object.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewString(FPDF_DOCUMENT document, FPDF_WIDESTRING value);

// Create a Name object (e.g. /Type).
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewName(FPDF_DOCUMENT document, FPDF_BYTESTRING value);

// Create a Boolean object.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewBoolean(FPDF_DOCUMENT document, FPDF_BOOL value);

// Create a Number object.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewNumber(FPDF_DOCUMENT document, float value);

// Create a Reference to an existing Object.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_NewReference(FPDF_DOCUMENT document, FPDF_OBJECT object);


// ----------------------------------------------------------------------------
// 4. Dictionary Operations
// ----------------------------------------------------------------------------

// Get value by Key. Returns NULL if not found.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_Dict_Get(FPDF_OBJECT dict, FPDF_BYTESTRING key);

// Set Key to Value.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Dict_Set(FPDF_OBJECT dict, FPDF_BYTESTRING key, FPDF_OBJECT value);

// Helper to set a String value directly.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Dict_SetString(FPDF_OBJECT dict, FPDF_BYTESTRING key, FPDF_WIDESTRING value);

// Remove Key.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Dict_Remove(FPDF_OBJECT dict, FPDF_BYTESTRING key);


// ----------------------------------------------------------------------------
// 5. Array Operations
// ----------------------------------------------------------------------------

// Get number of elements.
FPDF_EXPORT int FPDF_CALLCONV FPDF_Array_Count(FPDF_OBJECT array);

// Get element at index.
FPDF_EXPORT FPDF_OBJECT FPDF_CALLCONV FPDF_Array_Get(FPDF_OBJECT array, int index);

// Append element to the end.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Array_Add(FPDF_OBJECT array, FPDF_OBJECT value);



// ----------------------------------------------------------------------------
// 6. Stream Operations
// ----------------------------------------------------------------------------

// Get decoded data from a stream object.
// references: DecodeStreamMaybeCopyAndReturnLength
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_Stream_GetData(FPDF_OBJECT stream, void* buffer, unsigned long buflen, unsigned long* out_buflen);

#ifdef __cplusplus
}
#endif

#endif  // PUBLIC_FPDF_ATOMIC_H_
