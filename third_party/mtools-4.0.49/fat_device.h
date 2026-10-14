#ifndef MTOOLS_FAT_DEVICE_H
#define MTOOLS_FAT_DEVICE_H

/*  Copyright 2022 Alain Knaff.
 *  This file is part of mtools.
 *
 *  Mtools is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Mtools is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Mtools.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "msdos.h"
#include "device.h"
#include "stream.h"

Stream_t *find_device(char drive, int mode, struct device *out_dev,
		      union bootsector *boot,
		      char *name, int *media, mt_off_t *maxSize,
		      int *isRop);

#endif
