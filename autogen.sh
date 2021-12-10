
#!/bin/sh
#
# Copyright 2021 NVIDIA CORPORATIONS. All rights reserved.
#
# See COPYING in top-level directory.
#
# Additional copyrights may follow
#
# $HEADER$
#

# exit on any error
set -e

# set up libtool if not done already
test -f autotools/ltmain.sh || ( libtoolize || glibtoolize )

# do all the autotools setup, in the right order, as many times as necessary
autoreconf -i --force

# clean up temporary files
rm -rf autom4te.cache