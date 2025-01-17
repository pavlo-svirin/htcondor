/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#ifndef __STRING_SPACE_H__
#define __STRING_SPACE_H__

#include <unordered_map>
#include <string>

class StringSpace
{
public:

    StringSpace() {}
    ~StringSpace() { clear(); }

    /** Delete copy ctor and assignment operator; anyone trying
    to copy the StringSpace probably didn't really want to. 
    */
    StringSpace(const StringSpace&) = delete;
    StringSpace& operator= (const StringSpace) = delete;

    /** Acts just like strdup, but only allocates memory
    if the string does not already exists in the string space.
    This copies a null-terminated string and inserts it into
    the string space, or increments a reference count if the string
    is already there, and returns a pointer to the 
    shared (de-deplicated) string.  Do not change the
    string returned! Call free_dedup() to deallocate,
    not free()!  Do not cast the return value to remove the const-ness,
    it is const for a reason!
    */
    const char *strdup_dedup(const char *str);


    /** Free a pointer returned by StringSpace::strdup_dedup().
    Will decrease the reference count on this string, and deallocate the
    memory when reference count hits zero.  Returns the number of copies
    of this string still in the StringSpace.
    */
    int free_dedup(const char *str);

    /** Deallocate all strings in the StringSpace.  This means
    all pointers returned by strdup_dedup are invalid.
    */
    void clear() { ss_map.clear(); }


private:

    typedef struct ssentry {     
        char *pstr;
        unsigned int count;
        ssentry() : pstr(NULL), count(0) {}
        ~ssentry() { free(pstr); }
    } ssentry;

    std::unordered_map<std::string, ssentry> ss_map;
};




#endif //__STRING_SPACE_H__



