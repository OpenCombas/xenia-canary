group("third_party")
project("libjuice")
  uuid("914e6a31-785b-40fd-8986-76dea1b790fb")
  kind("StaticLib")
  language("C")
  links({
  })
  defines({
    "_LIB",
    'JUICE_STATIC'
  })
  filter({"configurations:Release", "platforms:Windows"})
    buildoptions({
      "/MT",
      
    })
  filter {}

  includedirs({
    "libjuice/include/juice",
  })
  files({
    "libjuice/include/juice",
    "libjuice/src/*.h",
    "libjuice/src/*.c",
  })
