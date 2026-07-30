#include <iostream>
#include "cppy3/cppy3.hpp"

static PyObject* hello(PyObject *self, PyObject *args, PyObject *keywds)
{
    const char *name = "";    
    static char *kwlist[] = {(char*)"name", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywds, "s", kwlist, &name))
        return NULL;

    printf("Hello! I am %s.\n", name);

    Py_RETURN_NONE;
}

static PyMethodDef EmbMethods[] = {
    {"hello", (PyCFunction)(void(*)(void))hello, METH_VARARGS | METH_KEYWORDS, "Say hello."},
    {NULL, NULL, 0, NULL} /* Sentinel */
};

static PyModuleDef EmbModule = {
    PyModuleDef_HEAD_INIT, "emb", NULL, -1, EmbMethods
};

static PyObject*
PyInit_emb(void)
{
    return PyModule_Create(&EmbModule);
}

int main(int argc, char *argv[])
{
    cppy3::Config config;
    config.builtin_modules.push_back({"emb", PyInit_emb});
    cppy3::Interpreter interpreter(config);
    cppy3::Namespace main = interpreter.main();

    std::cout << "Hey, type in command line, e.g. print(2+2*2)" << std::endl
              << std::endl;

    size_t i = 0;
    for (std::string line; std::getline(std::cin, line); i++)
    {
        try
        {
            const cppy3::Var result = main.eval(line);
            std::cout << std::endl
                      << "Out[#" << i << " " << result.type_name() << "] " << result.str() << std::endl;
        }
        catch (const cppy3::Error &e)
        {
            std::cerr << e.format() << std::endl;
        }
    }

    return 0;
}
