#!/bin/bash


TCPDUMP_FILTER='udp and not (dst port 7000 or src port 7000)'
TCPDUMP_OPTS="-i lo -n -q -ttttt"

#
# configure a system with 10 processes.
#
NUMPROCS=10
./config.sh $NUMPROCS

for cnt in {2,4,6,8,10} ; do  
    for alg in {lamport,ricart,singhal,suzuki}; do
        for ix in {1..5}; do

            echo "$cnt-of-$NUMPROCS processes, $alg. Run: $ix"
            
            # Start capture
            CAPTURE_FILE="./stats/$cnt-of-$NUMPROCS-$alg-run-$ix"
            sudo tcpdump $TCPDUMP_OPTS -Z "$(whoami)" "$TCPDUMP_FILTER"\ >& ${CAPTURE_FILE}.cap &
            sleep 0.2
            
            #start simulation
            ./start.sh $alg -c $cnt -t 10 > /dev/null
            sleep 0.2
            
            #save files
            mv supervisor.log ${CAPTURE_FILE}.log
            mv $alg.msc ${CAPTURE_FILE}.msc
            sleep 0.2


            #stop the capture
            TCPDUMP_PID=$(pgrep tcpdump)
            [ -n "$TCPDUMP_PID" ] && kill -SIGINT "$TCPDUMP_PID"
            
            #sleep a little
            sleep 0.5
        done
    done
done


