#!/usr/bin/perl -w
# -----------------------------------------------------------------------------

use strict;
use lib ($0 =~ m|^(.*/)| ? $1 : ".");
use GnumericTest;

&message ("Check that no-one uses allow-none");

my @hits = `find $topsrc -type f -name '*.[ch]' -print0 | xargs -0 -n100 grep -H '(allow-none)'`;

if (@hits) {
    print STDERR @hits;
    die "Fail.\n\n";
} else {
    print STDERR "Pass\n";
}
