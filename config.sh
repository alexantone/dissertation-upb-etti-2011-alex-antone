#!/bin/bash
NPROCS=3
CFGFILE="dme.conf"

[ -n "$1" ] && NPROCS=$1

cat > $CFGFILE <<EOD
$NPROCS # Number of interactng processes. This must be on the first line of this file!
 
#
# This is the supervisor's listening port. It must allways be before the list
# of processes
#
127.0.0.1:7000

#
# The list of listening ports for the processes
#

EOD

printlinkspeeds() {
    for (( jx = 1; jx <= NPROCS; jx++ )) ; do
                printf "10M "
    done
} ;

LINKSPEEDS=`printlinkspeeds`

for (( ix = 1; ix <= NPROCS; ix++ )) ; do
        printf "127.0.0.1:%d $LINKSPEEDS\n" $(( 9000 + ix )) >> $CFGFILE
done
