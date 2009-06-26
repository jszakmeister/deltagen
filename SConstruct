env = Environment()

env.ParseConfig("apu-1-config --ldflags --libs")
env.ParseConfig("apr-1-config --includes --cflags --ldflags --libs")
env.Append(CPPPATH = ['/usr/include/subversion-1'])

env.Program(target='deltagen',
            source=['deltagen.c'],
            LIBS=['svn_client-1', 'svn_delta-1', 'svn_subr-1', 'apr-1'])
