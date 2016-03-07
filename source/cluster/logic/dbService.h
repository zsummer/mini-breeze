﻿
/*
* breeze License
* Copyright (C) 2015 YaweiZhang <yawei.zhang@foxmail.com>.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/


/*
*  file desc 
*  db manager
*/




#ifndef _DB_MANAGER_H_
#define _DB_MANAGER_H_
#include <common.h>





class DBService : public Service
{
public:
    DBService();
    ~DBService();
    bool init();
    bool start();
    bool stop(std::function<void()> onSafeClosed);
public:
    void asyncQuery(const std::string &sql, const std::function<void(zsummer::mysql::DBResultPtr)> & handler);
    void asyncQuery(const std::string &sql);
    zsummer::mysql::DBResultPtr query(const std::string &sql);

public:

private:
    std::string _db;
    DBHelperPtr _dbHelper;
    zsummer::mysql::DBAsyncPtr _dbAsync;
    
private:
    void _checkSafeClosed();
    std::function<void()> _onSafeClosed;
};





































#endif