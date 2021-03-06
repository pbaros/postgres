#
# Script that provides 'make install' functionality for msvc builds
#
# $PostgreSQL$
#
use strict;
use warnings;

use Install qw(Install);

my $target = shift || Usage();
Install($target);

sub Usage
{
    print "Usage: install.pl <targetdir>\n";
    exit(1);
}
