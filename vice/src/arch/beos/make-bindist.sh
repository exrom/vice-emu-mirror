#!/bin/sh
# make-bindist.sh for the BEOS port
#
# written by Marco van den Heuvel <blackystardust68@yahoo.com>
#
# make-bindist.sh <strip> <vice-version> <cpu> <zip|nozip> <top-srcdir>
#                 $1      $2             $3    $4          $5

if [ ! -e src/x64 -o ! -e src/x128 -o ! -e src/xvic -o ! -e src/xpet -o ! -e src/xplus4 -o ! -e src/xcbm2 -o ! -e src/c1541 -o ! -e src/petcat -o ! -e src/cartconv ]
then
  echo Error: executable file\(s\) not found, do a \"make\" first
  exit 1
fi

echo Generating BEOS port binary distribution.
rm -f -r BeVICE-$2.$3
mkdir BeVICE-$2.$3
$1 src/x64
$1 src/x128
$1 src/xvic
$1 src/xpet
$1 src/xplus4
$1 src/xcbm2
$1 src/c1541
$1 src/petcat
$1 src/cartconv
cp src/x64 src/x128 src/xvic BeVICE-$2.$3
cp src/xpet src/xplus4 src/xcbm2 BeVICE-$2.$3
cp src/c1541 src/petcat src/cartconv BeVICE-$2.$3
cp -a data/C128 data/C64 data/CBM-II data/DRIVES BeVICE-$2.$3
cp -a data/PET data/PLUS4 data/PRINTER data/VIC20 BeVICE-$2
cp -a data/fonts BeVICE-$2.$3
mkdir BeVICE-$2.$3/doc
cp -a $5/doc/html BeVICE-$2.$3/doc
cp $5/doc/iec-bus.txt BeVICE-$2.$3/doc
cp $5/doc/mon.txt BeVICE-$2.$3/doc
cp $5/doc/cartconv.txt BeVICE-$2.$3/doc
cp $5/doc/Readme.beos BeVICE-$2.$3/doc
cp $5/FEEDBACK $4/README BeVICE-$2.$3
rm `find BeVICE-$2.$3 -name "Makefile*"`
rm `find BeVICE-$2.$3 -name "amiga_*.vkm"`
rm `find BeVICE-$2.$3 -name "dos_*.vkm"`
rm `find BeVICE-$2.$3 -name "os2*.vkm"`
rm `find BeVICE-$2.$3 -name "win_*.vkm"`
rm `find BeVICE-$2.$3 -name "x11_*.vkm"`
rm BeVICE-$2.$3/html/texi2html
if test x"$4" = "xzip"; then
  if test x"$ZIP" = "x"; then
    zip -r -9 -q BeVICE-$2.$3.zip BeVICE-$2.$3
  else
    $ZIP BeVICE-$2.$3.zip BeVICE-$2.$3
  fi
  rm -f -r BeVICE-$2.$3
  echo BEOS port binary distribution archive generated as BeVICE-$2.$3.zip
else
  echo BEOS port binary distribution directory generated as BeVICE-$2.$3
fi
