#include "pch.h"
#include <iostream>
#include <future>
#include "signalrclient/hub_connection.h"
#include "signalrclient/hub_connection_builder.h"
#include "signalrclient/signalr_client_config.h"
#include "signalrclient/web_exception.h"
#include "signalrclient/signalr_value.h"
#include <queue>
#include <windows.h>

#define LUA_LIB
#define LUA_BUILD_AS_DLL

extern "C" {
#include "./lauxlib.h"
#include "./lua.h"
}

class logger : public signalr::log_writer
{
    virtual void write(const std::string& entry) override
    {
        std::ofstream myfile;
        myfile.open("websocketLogging.txt", std::fstream::app);
        myfile << entry;
        myfile.close();
    }
};

std::queue<std::string> queue;
signalr::hub_connection connection = signalr::hub_connection_builder::create("http://localhost:5000/hub").build();
bool isConnectionSetup = false;
std::string url;
std::string token;

BOOL APIENTRY DllMain(HANDLE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    return TRUE;
}


static void handleDisconnected(std::exception_ptr ex)
{
    if (isConnectionSetup == false) {
        return;
    }

    std::promise<void> start_task;

    connection.start([&start_task](std::exception_ptr exception) {
        start_task.set_value();
        });

    connection.on("SendMessageToServer", [](const std::vector<signalr::value>& m)
        {
            queue.push(m[0].as_string());
        });

    start_task.get_future().get();
    return;
}

static int DisconnectWS(lua_State* L)
{
    std::promise<void> stop_task;
    connection.stop([&stop_task](std::exception_ptr exception) {
        stop_task.set_value();
        });

    stop_task.get_future().get();
    return 0;
}

static int GetMessageWS(lua_State* L)
{
    if (queue.empty()) {
        lua_pushstring(L, "");
        return 1;
    }

    auto out = queue.front();
    queue.pop();
    lua_pushstring(L, out.c_str());
    return 1;

}

static int SendMessageWS(lua_State* L)
{
    int numArgs = lua_gettop(L);

    if (numArgs == 2) {
        if (lua_isstring(L, 1)) {

            if (!lua_istable(L, 2)) {
                return luaL_error(L, "ConnectWS: Wait array string as 2 arg");
            }

            int n = lua_objlen(L, 2);
            std::vector<std::string> stringVector;

            for (int i = 1; i <= n; ++i) {
                // Получаем элемент массива по индексу
                lua_rawgeti(L, 2, i);

                // Проверяем, является ли элемент строкой
                if (!lua_isstring(L, -1)) {
                    return luaL_error(L, "ConnectWS: Ожидалась строка в массиве");
                }

                // Получаем строку и добавляем её в вектор
                const char* str = lua_tostring(L, -1);
                stringVector.push_back(str);

                // Удаляем элемент из стека
                lua_pop(L, 1);
            }
            const char* strArg = luaL_checkstring(L, 1);

            std::promise<void> send_task;
            std::vector<signalr::value> args;
            for (const std::string& str : stringVector) {
                args.push_back(signalr::value(str));
            }
            connection.invoke(strArg, args, [&send_task](const signalr::value& value, std::exception_ptr exception) {
                send_task.set_value();
                });

            send_task.get_future().get();
            lua_settop(L, 0);
            return 0;

        }
        else {
            return luaL_error(L, "ConnectWS: Argument must be a string");
        }
    }
    else {
        return luaL_error(L, "ConnectWS: Missing argument");
    }
}



static int ConnectWS(lua_State* L)
{
    int numArgs = lua_gettop(L);

    if (numArgs > 0 && numArgs < 3) {

        if (lua_isstring(L, 1))
        {
            url = lua_tostring(L, 1);
            std::promise<void> start_task;

            connection = signalr::hub_connection_builder::create(url)
                .with_logging(std::make_shared<logger>(), signalr::trace_level::error)
                .build();

            if (numArgs == 2)
            {
                if (lua_isstring(L, 2) == false)
                    return luaL_error(L, "ConnectWS: token must be string");

                token = lua_tostring(L, 2);
                signalr::signalr_client_config config;
                config.get_http_headers().emplace("Authorization", token);
                config.set_proxy({ web::web_proxy::use_auto_discovery });
                connection.set_client_config(config);
            }

            //connection.set_disconnected(handleDisconnected);

            connection.on("PendingMessage", [](const std::vector<signalr::value>& m)
            {
                    queue.push(m[0].as_string());
            });

            connection.start([&start_task](std::exception_ptr exception) {
                start_task.set_value();
                });

            start_task.get_future().get();
            isConnectionSetup = true;
        }
        else {
            return luaL_error(L, "ConnectWS: URL must be a string");
        }
    }
    else {
        return luaL_error(L, "ConnectWS: One or Two arguments excepted (url, token)");
    }

    return 0;
}

//Регистрация реализованных в dll функций, что бы те стали доступны из lua.
static struct luaL_reg ls_lib[] = {
  { "Connect", ConnectWS },
  { "SendMessage", SendMessageWS },
  { "GetMessage", GetMessageWS },
  { "Disconnect", DisconnectWS },
  { NULL, NULL }
};
//Эту функцию lua будет искать при подключении dll, её название заканчиваться названием dll, luaopen_ИМЯВАШЕЙDLL
extern "C" LUALIB_API int luaopen_WebS(lua_State * L) {
    luaL_openlib(L, "WebS", ls_lib, 0);
    return 0;
}