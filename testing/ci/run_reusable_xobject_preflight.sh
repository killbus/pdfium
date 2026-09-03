#!/usr/bin/env bash
set -euo pipefail

readonly EXPECTED_TESTS=(
  FPDFEditPageEmbedderTest.ReusableTextStampPreflightDoesNotAddObjects
  FPDFEditPageEmbedderTest.ReusableTextFormCanBeSharedAcrossPages
  FPDFEditPageEmbedderTest.ReusableTextFormsRetainSharedEmbeddedFontAfterClose
  FPDFEditPageEmbedderTest.ReusableTextFormRejectsFontOwnedByDifferentDocument
  FPDFEditPageEmbedderTest.ReusableTextFormRejectsInvalidInputs
  FPDFEditPageEmbedderTest.DeviceToPageByIndexMatchesLoadedCroppedRotatedPage
  FPDFViewEmbedderTest.BitmapRgbaPlacementsComposeWithoutMutatingPdfState
  FPDFViewEmbedderTest.BitmapRgbaPlacementsUseLoadedPageDisplayTransform
  FPDFViewEmbedderTest.BitmapRgbaPlacementsRejectInvalidInputBeforeDrawing
  FPDFViewEmbedderTest.BitmapPlacementsComposeCallerOwnedSourceWithoutMutatingPdfState
  FPDFViewEmbedderTest.BitmapPlacementsUseLoadedPageDisplayTransformAndDestinationByteOrder
  FPDFViewEmbedderTest.BitmapPlacementsRejectInvalidInputBeforeDrawing
  FPDFEditPageEmbedderTest.ReusableFormByIndexAppendsWithoutPageHandlesAndPersists
  FPDFEditPageEmbedderTest.ReusableFormByIndexIsolatesAppendFromOriginalClippingState
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsInvalidInputs
  FPDFEditPageEmbedderTest.ReusableFormByIndexClonesInitiallySharedResources
  FPDFEditPageEmbedderTest.ReusableImageByIndexAppendsWithoutPageHandleAndPersists
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsMalformedContentsWithoutResourceMutation
  FPDFEditPageEmbedderTest.ReusableFormByIndexPreservesSupportedContentsShapes
  FPDFEditPageEmbedderTest.ReusableFormByIndexClonesInheritedAncestorResources
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsCyclicParentDespiteDirectResources
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsParentTypeMutationDespiteDirectResources
  FPDFEditPageEmbedderTest.ReusableFormByIndexClonesIndirectXObjectSubdictionaryAndAvoidsCollision
  FPDFEditPageEmbedderTest.ReusableImageByIndexClonesIndirectExtGStateSubdictionaryAndAvoidsCollision
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsPageTreeMutation
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsDuplicatePageDictionaryIdentity
  FPDFEditPageEmbedderTest.ReusableFormByIndexRejectsSameCountPageReplacement
  FPDFPPOEmbedderTest.SequentialCompactImportsPreserveMetadataAfterFullMove
)
readonly FOCUSED_TEST_FILTER=$(IFS=:; printf '%s' "${EXPECTED_TESTS[*]}")

if [[ "${1:-}" == '--print-filter' ]]; then
  printf '%s\n' "$FOCUSED_TEST_FILTER"
  exit 0
fi

if (( $# != 0 )); then
  printf 'usage: %s [--print-filter]\n' "$0" >&2
  exit 2
fi

readonly SRC=${PDFIUM_SRC:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}
readonly CLIENT_ROOT=${PDFIUM_CLIENT_ROOT:-$(dirname "$SRC")}
readonly OUT=${NATIVE_OUT:-$SRC/out/native-reusable-xobject-preflight}
readonly GCLIENT_FILE=$CLIENT_ROOT/.gclient
readonly TEST_SOURCES=(
  "$SRC/fpdfsdk/fpdf_editpage_embeddertest.cpp"
  "$SRC/fpdfsdk/fpdf_ppo_embeddertest.cpp"
  "$SRC/fpdfsdk/fpdf_view_embeddertest.cpp"
)
readonly GN_ARGS='is_debug=false is_component_build=false pdf_is_standalone=true pdf_enable_v8=false pdf_enable_xfa=false pdf_use_skia=false pdf_enable_fontations=false pdf_use_partition_alloc=false clang_use_chrome_plugins=false treat_warnings_as_errors=false use_remoteexec=false symbol_level=0'

for command_name in git gclient gn autoninja; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'required command is unavailable: %s\n' "$command_name" >&2
    exit 1
  fi
done

for test_source in "${TEST_SOURCES[@]}"; do
  if [[ ! -f "$test_source" ]]; then
    printf 'embedder test source is missing: %s\n' "$test_source" >&2
    exit 1
  fi
done

for qualified_test_name in "${EXPECTED_TESTS[@]}"; do
  test_name=${qualified_test_name#*.}
  if ! grep -Fq "$test_name" "${TEST_SOURCES[@]}"; then
    printf 'expected native regression is absent from source: %s\n' \
      "$qualified_test_name" >&2
    exit 1
  fi
done

if [[ -e "$GCLIENT_FILE" ]]; then
  printf 'refusing to replace existing gclient configuration: %s\n' \
    "$GCLIENT_FILE" >&2
  exit 1
fi

readonly SOURCE_SHA_BEFORE=$(git -C "$SRC" rev-parse HEAD)
readonly SOURCE_URL=$(git -C "$SRC" remote get-url origin)
readonly SOLUTION_NAME=$(basename "$SRC")

cleanup() {
  rm -f "$GCLIENT_FILE"
}
trap cleanup EXIT

cat >"$GCLIENT_FILE" <<EOF
solutions = [
  {
    "name": "$SOLUTION_NAME",
    "url": "$SOURCE_URL",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
  },
]
EOF

(cd "$CLIENT_ROOT" && gclient sync --no-history --shallow)

readonly SOURCE_SHA_AFTER=$(git -C "$SRC" rev-parse HEAD)
if [[ "$SOURCE_SHA_AFTER" != "$SOURCE_SHA_BEFORE" ]]; then
  printf 'gclient changed the pinned PDFium revision: %s -> %s\n' \
    "$SOURCE_SHA_BEFORE" "$SOURCE_SHA_AFTER" >&2
  exit 1
fi

(cd "$SRC" && bash ./build/install-build-deps.sh --no-prompt)

printf 'PDFium SHA: %s\n' "$SOURCE_SHA_BEFORE"
printf 'GN args: %s\n' "$GN_ARGS"
printf 'Focused test filter: %s\n' "$FOCUSED_TEST_FILTER"

gn gen "$OUT" --root="$SRC" --args="$GN_ARGS"
autoninja -C "$OUT" pdfium_embeddertests

readonly LISTED_TESTS=$("$OUT/pdfium_embeddertests" \
  "--gtest_filter=$FOCUSED_TEST_FILTER" \
  --gtest_list_tests)

for qualified_test_name in "${EXPECTED_TESTS[@]}"; do
  test_name=${qualified_test_name#*.}
  if ! grep -Fq "$test_name" <<<"$LISTED_TESTS"; then
    printf 'focused filter did not select expected test: %s\n' \
      "$qualified_test_name" >&2
    exit 1
  fi
done

"$OUT/pdfium_embeddertests" \
  "--gtest_filter=$FOCUSED_TEST_FILTER" \
  --gtest_color=yes
