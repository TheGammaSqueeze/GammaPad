#!/bin/sh
# generate key_name_map.inc from input-event-codes.h
# Run this after any update to input-event-codes.h

HEADER="input-event-codes.h"
OUT="key_name_map.inc"

grep -E '^#define (BTN|KEY)_[A-Z0-9_]+' "$HEADER" \
  | awk '{print "    {\"" $2 "\", " $2 "},"}' \
  > "$OUT"
echo "Wrote $OUT ($(wc -l < "$OUT") entries)"
