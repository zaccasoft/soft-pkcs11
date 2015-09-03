
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <error.h>
#include <termios.h>
#include <sys/wait.h>

#include <iostream>
#include <system_error>

#include <openssl/bn.h>

#include "tools.h"
#include "exceptions.h"

int log_fd = 0;


void st_logf(const char* fmt, ...)
{
    if (log_fd == 0) {
        log_fd = ::open("/tmp/soft-token.log", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    }
    
    va_list ap;
    va_start(ap, fmt);
//     vdprintf(STDOUT_FILENO, fmt, ap);
    vdprintf(log_fd, fmt, ap);    
    va_end(ap); 
    
//     fsync(log_fd);
}

void print_attributes(const CK_ATTRIBUTE *attributes, CK_ULONG num_attributes)
{
    Attributes attrs;
    for (CK_ULONG i = 0; i < num_attributes; i++) {
        attrs[attributes[i].type] = attributes[i];
    }  
    print_attributes(attrs);
}

void print_attributes(const Attributes& attributes)
{
    CK_ULONG i;

    st_logf("Print attributes: %lu\n", attributes.size());

    for (auto it = attributes.begin(); it != attributes.end(); ++it) {
        const attribute_t& attr = it->second;
        
        switch (it->first) {
        case CKA_TOKEN: {
            st_logf("  A type: <CKA_TOKEN> size: <%d> value: <%s>\n", attr->ulValueLen, (attr.to_bool()) ? "TRUE" : "FALSE");
            break;
        }
        case CKA_CLASS: {
            CK_OBJECT_CLASS klass = attr.to_class();
            switch (klass) {
            case CKO_CERTIFICATE:
                st_logf("  A type: <CKA_CLASS> size: <%d> value: <%s>\n", attr->ulValueLen, "certificate");
                break;
            case CKO_PUBLIC_KEY:
                st_logf("  A type: <CKA_CLASS> size: <%d> value: <%s>\n", attr->ulValueLen, "public key");
                break;
            case CKO_PRIVATE_KEY:
                st_logf("  A type: <CKA_CLASS> size: <%d> value: <%s>\n", attr->ulValueLen, "private key");
                break;
            case CKO_SECRET_KEY:
                st_logf("  A type: <CKA_CLASS> size: <%d> value: <%s>\n", attr->ulValueLen, "secret key");
                break;
            case CKO_DOMAIN_PARAMETERS:
                st_logf("  A type: <CKA_CLASS> size: <%d> value: <%s>\n", attr->ulValueLen, "domain parameters");
                break;
            default:
                st_logf("  A type: <CKA_CLASS> size: <%d> value: [class 0x%08lx]\n", attr->ulValueLen, klass);
                break;
            }
            break;
        }
        case CKA_PRIVATE:
            st_logf("  A type: <CKA_PRIVATE> size: <%d>\n", attr->ulValueLen);
            break;
        case CKA_LABEL:
            st_logf("  A type: <CKA_LABEL> size: <%d>       value: <%s>\n", attr->ulValueLen, attr.to_string().c_str());
            break;
        case CKA_APPLICATION:
            st_logf("  A type: <CKA_APPLICATION> size: <%d> value: <%s>\n", attr->ulValueLen, attr.to_string().c_str());
            break;
        case CKA_VALUE:
            st_logf("  A type: <CKA_VALUE> size: <%d>\n", attr->ulValueLen);
            break;
        case CKA_ID:
            st_logf("  A type: <CKA_ID> size: <%d> value: <%lu>\n", attr->ulValueLen, attr.to_id());
            break;
        case CKA_OBJECT_ID:
            st_logf("  A type: <CKA_OBJECT_ID> size: <%d> value: <%s>\n", attr->ulValueLen, attr.to_object_id().c_str());

            break;
        default:
            st_logf("  A type: <UNKNOWN> size: <%d> type: [0x%08lx]\n", attr->ulValueLen, attr->type);
            break;
        }
    }
}

std::pair<int, std::shared_ptr<unsigned char>> read_bignum(void* ssl_bignum)
{
    BIGNUM *b = reinterpret_cast<BIGNUM*>(ssl_bignum);
    
    int size = BN_num_bytes(b);
    assert(size > 0);

    std::shared_ptr<unsigned char> buff(static_cast<unsigned char*>(malloc(size)), free);
    assert(buff.get() != NULL);

    int rc = BN_bn2bin(b, buff.get());
    assert(size == rc);
    return std::make_pair(size, buff);
}

std::vector<char> read_all(std::shared_ptr<FILE> file)
{
    std::vector<char> data;

    if (file) {
        std::vector<char> portion(4096);
        while(!::feof(file.get())) {
            portion.resize(4096);
            portion.resize(::fread(portion.data(), 1, portion.size(), file.get()));
            data.insert(data.end(), portion.begin(), portion.end());
        }
    }
    return data;
}

std::shared_ptr<FILE> read_mem(const std::vector< char >& data)
{
    return std::shared_ptr<FILE>(
        ::fmemopen(const_cast<char*>(data.data()), data.size(), "r"),
        ::fclose
    );
}

std::shared_ptr<FILE> write_mem(char **buf, size_t *size)
{
    return std::shared_ptr<FILE>(
        ::open_memstream(buf, size),
        ::fclose
    );
}


void set_stdin_echo(bool enable)
{
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    if( !enable )
        tty.c_lflag &= ~ECHO;
    else
        tty.c_lflag |= ECHO;

    (void) tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

std::string read_password()
{
    std::cout << "Input PIN:" << std::endl;
    set_stdin_echo(false);
    std::string pass;
    std::cin >> pass;
    set_stdin_echo(true);
    return pass;
}


std::vector<char> piped(const std::string& cmd, const std::vector<char>& input) {
    std::vector<char> result;
            
    int fd1[2];
    int fd2[2];
    int fd3[2];
    pid_t pid;

    if ( (pipe(fd1) < 0) || (pipe(fd2) < 0) || (pipe(fd3) < 0) )
    {
        throw std::system_error(errno, std::system_category(), "Can't create pipe to subprocess");
    }
    
    if ( (pid = fork()) < 0 )
    {
        throw std::system_error(errno, std::system_category(), "Can't create fork subprocess");
    }
    else if (pid == 0)     // CHILD PROCESS
    {
        close(fd1[1]);
        close(fd2[0]);
        close(fd3[0]);

        if (fd1[0] != STDIN_FILENO)
        {
            if (dup2(fd1[0], STDIN_FILENO) != STDIN_FILENO)
            {
                exit(EXIT_FAILURE);
            }
            close(fd1[0]);
        }

        if (fd2[1] != STDOUT_FILENO)
        {
            if (dup2(fd2[1], STDOUT_FILENO) != STDOUT_FILENO)
            {
                exit(EXIT_FAILURE);
            }
            close(fd2[1]);
        }
        
        if (fd3[1] != STDERR_FILENO)
        {
            if (dup2(fd3[1], STDERR_FILENO) != STDERR_FILENO)
            {
                exit(EXIT_FAILURE);
            }
            close(fd3[1]);
        }

        execlp("sh", "sh", "-c", cmd.c_str(), 0);
        exit(EXIT_FAILURE);
    }
    else
    {
        int rv;
        close(fd1[0]);
        close(fd2[1]);
        close(fd3[1]);

        if (input.size()) {
            if (write(fd1[1], input.data(), input.size()) != input.size())
            {
                throw std::system_error(errno, std::system_category(), "can't write to subprocess");
            }
        }
        
        close(fd1[1]);

        std::vector<char> portion(4096);
        
        while (true) {
            auto size = read(fd2[0], portion.data(), portion.size());
            if (size == -1) {
                throw std::system_error(errno, std::system_category(), "can't read from subprocess");
            } else if (size > 0) {
                portion.resize(size);
                result.insert(result.end(), portion.begin(), portion.end());
            } else {
                break;
            }
        }

        int exitcode;
        waitpid(pid, &exitcode, 0);
        if (WEXITSTATUS(exitcode) != 0) {
            throw std::runtime_error("process failed: " + cmd);
        }
    }
    
    return result;
}

int start(const std::string& cmd, const std::vector<char>& input) {
    FILE* file = NULL;
    try {
        file = ::popen(cmd.c_str(), "w");

        if (!file) {
            return -1;
        }

        if (input.size()) {
            if (::fwrite(input.data(), 1, input.size(), file) != input.size()) {
                pclose(file);
                return -1;
            }
        }
        return pclose(file);
    }
    catch(...) {
        pclose(file);
        return -1;
    }
}




