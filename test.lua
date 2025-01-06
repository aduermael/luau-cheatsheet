--!strict

print("hello from Luau!")

local i: number = 41 -- typed variable
i += 1 -- += operator

print("i: " .. i)

-- i = "foo" -- analyze ERROR
-- print("i: " .. i)

-- type A = {x: number, y: number, z: number?}
-- type B = {x: number, y: number, z: number}

-- local a1: A = {x = 1, y = 2}        -- ok
-- local b1: B = {x = 1, y = 2, z = 3} -- ok

-- local a2: A = b1 -- ok
-- local b2: B = a1 -- not ok

local onlyString: string = 42

-- type TestType = { msg: string }

-- local tt: TestType = { msg = "foo"}

-- print(tt)

-- local tt2 = Instance.new("TestType")

-- print(tt2:IsA("TestType"))
