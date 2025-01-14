--!strict

print("hello from Luau!")

local i: number = 41 -- typed variable
i += 1 -- += operator

print("i: " .. i)

-- test type checking with error:
-- local str: string = 100
-- print(str)

-- test function
-- function foo()
--     return i + "coucou"
-- end

type config = {
    a: number,
    b: string
}

function foo(config: config)
    print(config.b)
end

foo({a = 1, b = "coucou"})

-- print("require:", require)
local m = require("./module")
-- local m = require("module")
-- print(m)
m.foo()