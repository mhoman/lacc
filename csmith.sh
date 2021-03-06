#!/bin/bash

directory="csmith"
csmith_options="--no-packed-struct --float"
csmith_include="$1"
compiler="$2"
if [[ -z "$csmith_include" || -z "$compiler" ]]
then
	echo "Usage: $0 <csmith include path> <reference compiler>"
	exit
fi

n=1
while [ true ]
do
	filename="${directory}/${n}.c"
	program="${directory}/${n}"
	csmith ${csmith_options} > ${filename}

	${compiler} -w -std=c99 -I ${csmith_include} ${filename} -o ${program}
	timeout 1 ${program} > /dev/null
	if [ $? -ne 0 ]
	then
		rm ${program}
		continue
	fi

	rm ${program}
	./check.sh \
		"bin/lacc -w -std=c99 -I ${csmith_include}" ${filename} \
		"${compiler} -w -std=c99 -I ${csmith_include}"
	if [ $? -ne 0 ]
	then
		break
	fi

	n=$((n + 1))
done
