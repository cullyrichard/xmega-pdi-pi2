/* Copyright (C) 2015 DiUS Computing Pty. Ltd.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef _PAGE_MAP_H_
#define _PAGE_MAP_H_

#include <map>
#include <cstdint>
#include <cstring>


template<unsigned PAGE_SIZE>
struct page_t
{
  uint32_t addr;
  char data[PAGE_SIZE];

  page_t () : addr (0) { memset (data, 0xff, PAGE_SIZE); }
  
  bool operator == (const page_t &o) const { return addr == o.addr; }
  bool operator <  (const page_t &o) const { return addr <  o.addr; }

  typedef typename std::map<uint32_t, page_t<PAGE_SIZE> > container_t;
};


#endif
