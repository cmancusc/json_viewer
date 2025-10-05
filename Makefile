# As summary, use : gcc ( flags ) -o name ( include et linking options)

CC = gcc
C_STANDARD = -std=gnu11
INCLUDE_DIR = include
SOURCES_DIR = src
BUILD_DIR = build
APPLICATION_NAME = jsonViewer
FILENAME = ${BUILD_DIR}/${APPLICATION_NAME}
CFLAGS = -Wall -ansi -pedantic-errors ${C_STANDARD}
LDFLAGS = -lncurses
DEBUG_SUFFIX = _debug
# VERBOSE = -v
CFLAGS_DEBUG = ${VERBOSE} -fsanitize=address -static-libasan -gdwarf-2 -DDEBUG
OBJS = ${SOURCES_DIR}/*.c

all : ${FILENAME} ${FILENAME}${DEBUG_SUFFIX}


${FILENAME}: ${OBJS}
		${CC} ${CFLAGS} -o $@  $^ -I${INCLUDE_DIR} ${LDFLAGS}

${FILENAME}${DEBUG_SUFFIX}: ${OBJS}
		${CC} ${CFLAGS} ${CFLAGS_DEBUG} -o $@ $^ -I${INCLUDE_DIR} ${LDFLAGS}


clean:
		${RM} *.o ${FILENAME} ${FILENAME}${DEBUG_SUFFIX}

configure:
		mkdir -p ${BUILD_DIR}
		mkdir -p ${INCLUDE_DIR}
		mkdir -p ${SOURCES_DIR}
