#!/bin/bash

cd ../build

# compile

make tokenminer

if [ $? -ge 1 ] ; then
   echo "."
   echo "."
   echo "."
   echo "compilation error"
   echo "."
   echo "."
   echo "."
   exit 1
fi

cp ethminer/tokenminer ../release/stage
cp ../tokenminer.ini ../release/stage
cp ../README.md ../release/stage

cd ../release

rm *.tar.gz
tar -czf mvis-tokenminer-version-linux.tar.gz  --directory=stage .

if [ $? -ge 1 ] ; then
   echo "."
   echo "."
   echo "."
   echo "ERRORS WERE ENCOUNTERED!!!"
   echo "."
   echo "."
   echo "."
   exit 1
fi

echo "."
echo "."
echo "."
echo "script completed successfully"
echo "."
echo "."
echo "."
