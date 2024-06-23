local apteryx = require('apteryx')

local function reboot(path, input)
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

local function get_reboot_info(path, input)
    local info = apteryx.get_tree("/system/reboot-info")
    return {
        ["reboot-time"] = info["reboot-time"] or nil,
        ["message"] = info["message"] or nil,
        ["language"]  = info["language"] or nil,
    }
end

local function reset_state(path, input)
    local delay = input["delay"] 
    apteryx.set("/t4:test/state/age", delay)
    return true
end

local function get_reset_time(path, input)
    return { ["last-reset"] = apteryx.get("/t4:test/state/age") }
end

local function set_age(path, input)
    local user = path:match('/t4:test/state/users/([^/]+)')
    local age = input["age"] 
    apteryx.set("/t4:test/state/users/" .. user .. "/age", age)
    return true
end

return {
    ["/operations/t4:reboot"] = reboot,
    ["/operations/t4:get-reboot-info"] = get_reboot_info,
    ["/t4:test/state/reset"] = reset_state,
    ["/t4:test/state/get-last-reset-time"] = get_reset_time,
    ["/t4:test/state/users/*/set-age"] = set_age,
}
