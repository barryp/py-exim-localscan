/*
 * Python wrapper for Exim local_scan feature
 *
 * Building on FreeBSD 4.x:  edit Local/Makefile
 * and add these lines (if already have CFLAGS/EXTRALIBS, just append this stuff)
 * 
 *  LOCAL_SCAN_SOURCE=Local/expy_local_scan.c
 *  CFLAGS= -I/usr/local/include/python2.2
 *  EXTRALIBS= -lm -lutil -pthread /usr/local/lib/python2.2/config/libpython2.2.a -Wl,--export-dynamic
 * 
 *
 * 2002-10-20  Barry Pederson <bp@barryp.org>
 *
 */

#include <Python.h>
#include "local_scan.h"

/* ---- Tweakable settings -------- 
 
 This code will act *somewhat* like this python-ish pseudocode:
   
   try:
       import USER_MODULE_NAME 

       rc = USER_MODULE_NAME.USER_FUNCTION_NAME()

       if rc is sequence:
           if len(rc) > 1:
               return_text = str(rc[1])
           rc = rc[0]

       assert rc is integer
       return rc
   except:
       return_text = "some description of problem"
       return PYTHON_FAILURE_RETURN
   
 And a do-nothing USER_MODULE_NAME.py file might look like:

   import BUILTIN_MODULE_NAME

   def USER_FUNCTION_NAME():
       return BUILTIN_MODULE_NAME.LOCAL_SCAN_ACCEPT

*/
#define BUILTIN_MODULE_NAME     "exim"
#define USER_MODULE_NAME        "exim_local_scan"
#define USER_FUNCTION_NAME      "local_scan"
#define PYTHON_FAILURE_RETURN   LOCAL_SCAN_ACCEPT

/* ------- Globals ------------ */

static PyObject *expy_exim_dict = NULL;
static PyObject *expy_user_module = NULL;


/* ------- Custom type for holding header lines ------ 

  Basically a two-item sequence in which only the second item (the header line type)
  is changeable, and even then, limited to single-character strings

*/

typedef struct
    {
    PyObject_HEAD
    header_line *hline;
    } expy_header_line_t;


/* Header lines are always 2 items long (value, type) */
static int expy_header_line_length(PyListObject *a)
    {
    return 2;
    }


static PyObject * expy_header_line_item(expy_header_line_t *self, int i)
    {
    char ch;

    switch (i)
        {
        case 0:
            return PyString_FromString(self->hline->text);

        case 1:
            ch = (char)(self->hline->type);
            return PyString_FromStringAndSize(&ch, 1);

        default:
	    PyErr_SetString(PyExc_IndexError, "list index out of range");
            return NULL;
        }
    }


static int expy_header_line_ass(expy_header_line_t *self, int i, PyObject *value)
    {
    char *p;
    int len;

    switch (i)
        {
        case 0:
	    PyErr_SetString(PyExc_TypeError, "value of header line can't be changed");
            return -1;

        case 1:
            if (PyString_AsStringAndSize(value, &p, &len) == -1)
                return -1;

            if (len != 1)
                {
		PyErr_SetString(PyExc_TypeError, "type of header line must be single character");
                return -1;
                }

            self->hline->type = (int)(p[0]);
            return 0;

        default:
	    PyErr_SetString(PyExc_IndexError, "list assignment index out of range");
            return -1;
        }
    }

 
static PySequenceMethods header_line_as_sequence =
    {
    (inquiry) expy_header_line_length,
    0,
    0,
    (intargfunc) expy_header_line_item,
    0,
    (intobjargproc) expy_header_line_ass,
    };



static PyObject * expy_header_line_getattr(expy_header_line_t *self, char *name)
    {
    if (!strcmp(name, "text"))
        return PyString_FromString(self->hline->text);

    if (!strcmp(name, "type"))
        {
        char ch = (char)(self->hline->type);
        return PyString_FromStringAndSize(&ch, 1);
        }

    PyErr_Format(PyExc_AttributeError, "Unknown attribute: %s", name);
    return NULL;
    }


static int expy_header_line_setattr(expy_header_line_t *self, char *name, PyObject *value)
    {
    if (!strcmp(name, "type"))
        {
        char *p;
        int len;

        if (PyString_AsStringAndSize(value, &p, &len) == -1)
            return -1;

        if (len != 1)
            {
            PyErr_SetString(PyExc_TypeError, "header.type can only be set to a single-character value");
            return -1;
            }

        self->hline->type = (int)(p[0]);
        return 0;
        }

    PyErr_Format(PyExc_AttributeError, "Attribute: %s is not settable", name);
    return -1;
    }


static PyTypeObject ExPy_Header_Line  = 
    {
    PyObject_HEAD_INIT(&PyType_Type) 
    0,				/*ob_size*/
    "ExPy Header Line",		/*tp_name*/
    sizeof(expy_header_line_t),	/*tp_size*/
    0,			        /*tp_itemsize*/
    0,                          /*tp_dealloc*/
    0,                          /*tp_print*/
    (getattrfunc) expy_header_line_getattr,  /*tp_getattr*/
    (setattrfunc) expy_header_line_setattr,  /*tp_setattr*/
    0,			        /*tp_compare*/
    0,                          /*tp_repr*/
    0,				/*tp_as_number*/
    &header_line_as_sequence	/*tp_as_sequence*/
    };


PyObject * expy_create_header_line(header_line *p)
    {
    expy_header_line_t * result;

    result = (expy_header_line_t *) PyObject_NEW(expy_header_line_t, &ExPy_Header_Line);
    if (!result)
        return NULL;

    result->hline = p;

    return (PyObject *) result;
    }


/* ------- Helper functions for Module methods ------- */

/*
 * Given a C string, make sure it's safe to pass to a 
 * printf-style function ('%' chars are escaped by doubling 
 * them up.  Optionally, also make sure the string ends with a '\n'
 */
static char *get_format_string(char *str, int need_newline)
    {
    char *p;
    char *q;
    char *newstr;
    int percent_count;
    int len;

    /* Count number of '%' characters in string, and get the total length while at it */ 
    for (p = str, percent_count = 0; *p; p++)
        if (*p == '%')
            percent_count++;
    len = p - str;

    /* Decide if we need a newline added */
    if (need_newline)
        {
        if (len && (*(p-1) == '\n'))
            need_newline = 0;
        else
            need_newline = 1; /* paranoia - just in case something other than 1 was used to indicate truth */
        }

    /* If it's all good, just return the string we were passed */
    if ((!percent_count) && (!need_newline))
        return str;

    /* Gotta make a new string, with '%'s and/or '\n' added */
    newstr = store_get(len + percent_count + need_newline + 1);

    for (p = str, q = newstr; *p; p++, q++)
        {
        *q = *p;
        if (*q == '%')
            {
            q++;   
            *q = '%';
            }
        }

    if (need_newline)
        {
        *q = '\n';
        q++;
        }

    *q = 0;

    return newstr;
    }

/* -------- Module Methods ------------ */

/*
 * Have Exim do a string expansion, will raise
 * a Python ValueError exception if the expansion fails
 */
static PyObject *expy_expand_string(PyObject *self, PyObject *args)
    {
    char *str;
    char *result;

    if (!PyArg_ParseTuple(args, "s", &str))
        return NULL;
 
    result = expand_string(str);
    
    if (!result)
        {
        PyErr_Format(PyExc_ValueError, "expansion [%s] failed: %s", str, expand_string_message);
        return NULL;
        }
    
    return PyString_FromString(result);
    }


/*
 * Add a header line, will automatically tack on a '\n' if necessary
 */
static PyObject *expy_header_add(PyObject *self, PyObject *args)
    {
    char *str;

    if (!PyArg_ParseTuple(args, "s", &str))
        return NULL;
 
    header_add(' ', get_format_string(str, 1));

    Py_INCREF(Py_None);
    return Py_None;
    }


/*
 * Write to exim log, uses LOG_MAIN by default
 */
static PyObject *expy_log_write(PyObject *self, PyObject *args)
    {
    char *str;
    int which = LOG_MAIN;
    
    if (!PyArg_ParseTuple(args, "s|i", &str, &which))
        return NULL;
 
    log_write(0, which, get_format_string(str, 0));

    Py_INCREF(Py_None);
    return Py_None;
    }

/*
 * Print through Exim's debug_print() function, which does nothing if
 * Exim isn't in debugging mode. 
 */
static PyObject *expy_debug_print(PyObject *self, PyObject *args)
    {
    char *str;
    int which = LOG_MAIN;
    
    if (!PyArg_ParseTuple(args, "s", &str))
        return NULL;
 
    debug_printf(get_format_string(str, 0));

    Py_INCREF(Py_None);
    return Py_None;
    }


static PyMethodDef expy_exim_methods[] = 
    {        
    {"expand", expy_expand_string, METH_VARARGS, "Have exim expand string."},
    {"log", expy_log_write, METH_VARARGS, "Write message to exim log."},
    {"add_header", expy_header_add, METH_VARARGS, "Add header to message."},
    {"debug_print", expy_debug_print, METH_VARARGS, "Print if Exim is in debugging mode, otherwise do nothing."},
    {NULL, NULL, 0, NULL}
    };


/* ------------  Helper Functions for local_scan ---------- */

/*
 * Add a string to the module dictionary 
 */
static void expy_dict_string(char *key, uschar *val)
    {
    PyObject *s;

    if (val)
        s = PyString_FromString(val);
    else
        {
        s = Py_None;
        Py_INCREF(s);
        }

    PyDict_SetItemString(expy_exim_dict, key, s);
    Py_DECREF(s);
    }

/*
 * Add an integer to the module dictionary 
 */
static void expy_dict_int(char *key, int val)
    {
    PyObject *i;
    
    i = PyInt_FromLong(val);
    PyDict_SetItemString(expy_exim_dict, key, i);
    Py_DECREF(i);
    }

/*
 * Convert Exim header linked-list to Python tuple
 * of tuples.  Each inner tuple is 2 elements: header-text, and 
 * header-type, where header-type is a one-char code exim uses
 * to identify certain header lines (see chapter 48 of exim manual)
 */
static void expy_get_headers()
    {
    int header_count;  
    header_line *p;
    PyObject *result;
    char linetype;

    /* count number of header lines */
    for (header_count = 0, p = header_list; p; p = p->next)
        header_count++;

    /* Build up the tuple of tuples */
    result = PyTuple_New(header_count);
    for (header_count = 0, p = header_list; p; p = p->next)
        {
        PyTuple_SetItem(result, header_count, expy_create_header_line(p));
        header_count++;
        }

    /* Stick in dict and drop our reference */
    PyDict_SetItemString(expy_exim_dict, "headers", result);
    Py_DECREF(result);
    }


/*
 * Make tuple containing message recipients
 */
static PyObject *get_recipients()
    {
    PyObject *result;
    int i;

    result = PyTuple_New(recipients_count);
    for (i = 0; i < recipients_count; i++)
        PyTuple_SetItem(result, i, PyString_FromString(recipients_list[i].address));

    return result;
    }

/*
 * shift entries in list down to overwrite
 * entry in slot n (0-based)
 */
static void expy_remove_recipient(int n)
    {
    int i;
    for (i = n; i < (recipients_count-1); i ++)
        recipients_list[i] = recipients_list[i+1];

    recipients_count--;
    }


/* ----------- Actual local_scan function ------------ */

int local_scan(int fd, uschar **return_text)
    {
    PyObject *user_dict;
    PyObject *user_func;
    PyObject *result;
    PyObject *original_recipients;
    PyObject *working_recipients;

    if (!Py_IsInitialized())  /* maybe some other exim add-on already initialized Python? */
        Py_Initialize();

    if (!expy_exim_dict)
        {
        PyObject *module = Py_InitModule(BUILTIN_MODULE_NAME, expy_exim_methods); /* borrowed ref */
        expy_exim_dict = PyModule_GetDict(module);         /* borrowed ref */
        Py_INCREF(expy_exim_dict);                         /* want to keep it for later */
        }

    if (!expy_user_module)
        {
        expy_user_module = PyImport_ImportModule(USER_MODULE_NAME);

        if (!expy_user_module)
            {
            *return_text = "Internal error, missing module";
            log_write(0, LOG_REJECT, "Couldn't import 'expy_local_scan' module"); 
            return PYTHON_FAILURE_RETURN;
            }
        }

    user_dict = PyModule_GetDict(expy_user_module);  /* Borrowed Reference, never fails */

    user_func = PyMapping_GetItemString(user_dict, USER_FUNCTION_NAME);
    if (!user_func)
        {
        *return_text = "Internal error, missing function";
        log_write(0, LOG_REJECT, "Python expy_local_scan module doesn't have a 'local_scan' function"); 
        return PYTHON_FAILURE_RETURN;
        }

    /* so far so good, prepare to run function */

    /* Copy exim variables */
    expy_dict_int("debug_selector", debug_selector);
    expy_dict_int("host_checking", host_checking);
    expy_dict_string("interface_address", interface_address);
    expy_dict_int("interface_port", interface_port);
    expy_dict_string("message_id", message_id);
    expy_dict_string("received_protocol", received_protocol);
    expy_dict_string("sender_address", sender_address);
    expy_dict_string("sender_host_address", sender_host_address);
    expy_dict_string("sender_host_authenticated", sender_host_authenticated);
    expy_dict_string("sender_host_name", sender_host_name);
    expy_dict_int("sender_host_port", sender_host_port);
    expy_dict_int("fd", fd);

    /* copy some constants */
    expy_dict_int("LOG_MAIN", LOG_MAIN);
    expy_dict_int("LOG_PANIC", LOG_PANIC);
    expy_dict_int("LOG_REJECT", LOG_REJECT);
    
    expy_dict_int("LOCAL_SCAN_ACCEPT", LOCAL_SCAN_ACCEPT);
    expy_dict_int("LOCAL_SCAN_ACCEPT_FREEZE", LOCAL_SCAN_ACCEPT_FREEZE);
    expy_dict_int("LOCAL_SCAN_ACCEPT_QUEUE", LOCAL_SCAN_ACCEPT_QUEUE);
    expy_dict_int("LOCAL_SCAN_REJECT", LOCAL_SCAN_REJECT);
    expy_dict_int("LOCAL_SCAN_REJECT_NOLOGHDR", LOCAL_SCAN_REJECT_NOLOGHDR);
    expy_dict_int("LOCAL_SCAN_TEMPREJECT", LOCAL_SCAN_TEMPREJECT);
    expy_dict_int("LOCAL_SCAN_TEMPREJECT_NOLOGHDR", LOCAL_SCAN_TEMPREJECT_NOLOGHDR);
    expy_dict_int("MESSAGE_ID_LENGTH", MESSAGE_ID_LENGTH);
    expy_dict_int("SPOOL_DATA_START_OFFSET", SPOOL_DATA_START_OFFSET);

    expy_dict_int("D_v", D_v);
    expy_dict_int("D_local_scan", D_local_scan);

    /* set the headers */
    expy_get_headers();

    /* 
     * make list of recipients, give module a copy to work with in 
     * List format, but keep original tuple to compare against later
     */
    original_recipients = get_recipients();
    working_recipients = PySequence_List(original_recipients);
    PyDict_SetItemString(expy_exim_dict, "recipients", working_recipients);
    Py_DECREF(working_recipients);    

    /* Try calling our function */
    result = PyObject_CallFunction(user_func, NULL);
    Py_DECREF(user_func);  /* Don't need ref to function anymore */      

    /* Check for Python exception */
    if (!result)
        {
        PyErr_Clear();
        *return_text = "Internal error, local_scan function failed";
        Py_DECREF(original_recipients);
        return PYTHON_FAILURE_RETURN;

        // FIXME: should write exception to exim log somehow
        }

    /* User code may have replaced recipient list, so re-get ref */
    working_recipients = PyDict_GetItemString(expy_exim_dict, "recipients"); /* borrowed ref */

    /* 
     * reconcile original recipient list with what's present after 
     * Python code is done 
     */
    if ((!working_recipients) || (!PySequence_Check(working_recipients)) || (PySequence_Size(working_recipients) == 0))
        /* Python code either deleted exim.recipients alltogether, or replaced 
           it with a non-list, or emptied out the list */
        recipients_count = 0;
    else
        {        
        int i;

        /* remove original recipients not on the working list, reverse order important! */
        for (i = recipients_count - 1; i >= 0; i--)
            {
            PyObject *addr = PyTuple_GET_ITEM(original_recipients, i); /* borrowed ref */
            if (!PySequence_Contains(working_recipients, addr))
                expy_remove_recipient(i);
            }

        /* add new recipients not in the original list */
        for (i = PySequence_Size(working_recipients) - 1; i >= 0; i--)
            {
            PyObject *addr = PySequence_GetItem(working_recipients, i);
            if (!PySequence_Contains(original_recipients, addr))
                receive_add_recipient(PyString_AsString(addr), -1);
            Py_DECREF(addr);
            }
        }

    Py_DECREF(original_recipients);  /* No longer needed */

    /* Deal with the return value, first see if python returned a non-empty sequence */
    if (PySequence_Check(result) && (PySequence_Size(result) > 0))
        {
        /* save first item */
        PyObject *rc = PySequence_GetItem(result, 0);

        /* if more than one item, convert 2nd item to string and use as return text */
        if (PySequence_Size(result) > 1)
            {
            PyObject *str;
            PyObject *obj = PySequence_GetItem(result, 1);
            str = PyObject_Str(obj);
            Py_DECREF(obj);
            *return_text = string_copy(PyString_AsString(str));
            Py_DECREF(str);
            }

        /* drop the sequence, and focus on the first item we saved */
        Py_DECREF(result);
        result = rc;
        }

    /* If we have an integer, return that to Exim */
    if (PyInt_Check(result))
        {
        int rc = PyInt_AsLong(result);
        Py_DECREF(result);
        return rc;
        }

    /* didn't return anything usable */
    Py_DECREF(result);
    *return_text = "Internal error, bad return code";
    log_write(0, LOG_REJECT, "Python expy_local_scan module didn't return integer"); 
    return PYTHON_FAILURE_RETURN;
    }


/*-------- EOF --------------*/
