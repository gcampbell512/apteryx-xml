-- luacheck: globals _RPCS
local apteryx = require('apteryx')

local function reboot(input)
    if input["language"] ~= nil then
        -- generate results for test cases
        if input["language"] ~= 'en-US' then
            if input["language"]:find("-") then
                return false, "do not support language " .. input["language"]
            elseif input["language"] == "" then
                return false, false, false
            else
                return false
            end
        end
    end
    apteryx.set_tree ("/system/reboot-info", {
        ["reboot-time"]  = input["delay"] or nil,
        ["message"]  = input["message"] or nil,
        ["language"]  = input["language"] or nil,
    })
    return true
end

local function get_reboot_info()
    local info = apteryx.get_tree("/system/reboot-info")
    return {
        ["reboot-time"] = info["reboot-time"] or nil,
        ["message"] = info["message"] or nil,
        ["language"]  = info["language"] or nil,
    }
end

local function get_rpcs()
    if _RPCS ~= nil then
        return { paths=_RPCS }
    end
    return { paths={} }
end

local function reset_state(input, _, method)
    if method == "GET" then
        return { ["delay"] = apteryx.get("/t4:test/state/age") }
    elseif method == "POST" then
        local delay = input["delay"]
        apteryx.set("/t4:test/state/age", delay)
        return true
    end
    return false, "unsupported operation"
end

local function get_reset_time()
    return { ["last-reset"] = apteryx.get("/t4:test/state/age") }
end

local function get_reset_history()
    local lasttime = apteryx.get("/t4:test/state/age")
    local history = apteryx.get_tree("/t4:test/state/history")
    return { ["last-reset"] = lasttime, ["history"] = history }
end

local function createget_users(input, _, method)
    if method == "GET" then
        local users = {}
        for _, info in pairs(apteryx.get_tree('/t4:test/state/users')) do
            users[#users + 1] = {
                ["name"] = info["name"],
                ["age"] = info["age"],
            }
        end
        return {
            ["users"] = users,
        }
    elseif method == "POST" then
        if input["name"] == nil or input["age"] == nil then
            return false
        end
        apteryx.set_tree ("/t4:test/state/users/" .. input["name"], {
            ["name"] = input["name"],
            ["age"] = input["age"],
        })
    end
    return true
end

local function modifydelete_user(input, path, method)
    local user = path:match('/t4:test/state/users/([^/]+)')
    if user == nil then
        return false
    end
    if method == "PUT" then
        if input["age"] == nil or apteryx.get("/t4:test/state/users/" .. user .. '/name') ~= user then
            return false
        end
        apteryx.set_tree("/t4:test/state/users/" .. user, {
            ["age"]  = input["age"],
        })
        return true
    elseif method == "DELETE" then
        apteryx.prune("/t4:test/state/users/" .. user)
        return true
    end
    return false
end

local function set_user_age(input, path)
    local user = path:match('/t4:test/state/users/([^/]+)')
    if user == nil then
        return false
    end
    apteryx.set("/t4:test/state/users/" .. user .. "/age", input["age"])
    return true
end

return {
    { path="/operations/t4:reboot", methods={"POST"}, handler=reboot },
    { path="/operations/t4:get-reboot-info", methods={"GET", "POST"}, handler=get_reboot_info },
    { path="/operations/t4:get-rpcs", methods={"GET", "POST"}, handler=get_rpcs },
    { path="/t4:test/state/reset", methods={"GET", "POST"}, handler=reset_state },
    { path="/t4:test/state/get-last-reset-time", methods={"GET", "POST"}, handler=get_reset_time },
    { path="/t4:test/state/get-reset-history", methods={"GET", "POST"}, handler=get_reset_history },
    { path="/t4:test/state/users", methods={"GET", "POST"}, handler=createget_users },
    { path="/t4:test/state/users/*", methods={"PUT", "DELETE"}, handler=modifydelete_user },
    { path="/t4:test/state/users/*/set-age", methods={"POST"}, handler=set_user_age },
}
