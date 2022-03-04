#!/bin/sh

# GNU awk requires --non-decimal-data, but most don't..
AWK="awk"
if $AWK --non-decimal-data '' < /dev/null > /dev/null 2>&1; then
	AWK="$AWK --non-decimal-data"
fi


cat <<EOF
// SPDX-License-Identifier: MIT
// GENERATED FILE!

#ifndef BUTTOND_KEYNAMES_H
#define BUTTOND_KEYNAMES_H

struct keyname {
	const char *const name;
	const uint16_t code;
};

static const char *const keynames[] = {
EOF
$AWK '/define KEY_/ { $3=$3+0; if ($3) { print $2, $3+0; }}' < /usr/include/linux/input-event-codes.h | \
	sed -ne 's/^KEY_\(.*\) \([0-9]*\)$/	[\2] = "\1",/p'
cat <<EOF
};

#endif
EOF
