/*
 * Python wrapper for Exim local_scan feature
 *
 * 2002-10-20  Barry Pederson <bp@barryp.org>
 *
 */

#include <Python.h>
#include "local_scan.h"

/* ---- Settings controllable at runtime through Exim 'configure' file -------- 
 
 This local_scan module will act *somewhat* like this python-ish pseudocode:
   
   try:
       if {expy_path_add}:
           import sys
           sys.path.append({expy_path_add})

       import {expy_scan_module} 

       rc = {expy_scan_module}.{expy_scan_function}()

       if rc is sequence:
           if len(rc) > 1:
               return_text = str(rc[1])
           rc = rc[0]

       assert rc is integer
       return rc
   except:
       return_text = "some description of problem"
       return PYTHON_FAILURE_RETURN
   
 And a do-nothing {expy_scan_module}.py file might look like:

   import {expy_exim_module}

   def {expy_scan_function}():
       return {expy_exim_module}.LOCAL_SCAN_ACCEPT

*/

static BOOL    expy_enabled = TRUE;
static uschar *expy_path_add = NULL;
static uschar *expy_exim_module = US"exim";
static uschar *expy_scan_module = US"exim_local_scan";
static uschar *expy_scan_function = US"local_scan";
#define PYTHON_FAILURE_RETURN   LOCAL_SCAN_ACCEPT

optionlist local_scan_options[] = 
    {
    { "expy_enabled", opt_bool, &expy_enabled},
    { "expy_exim_module",  opt_stringptr, &expy_exim_module },
    { "expy_path_add",  opt_stringptr, &expy_path_add },
    { "expy_scan_function",  opt_stringptr, &expy_scan_function },
    { "expy_scan_module",  opt_stringptr, &expy_scan_module },
    };

int local_scan_options_count = sizeof(local_scan_options)/sizeof(optionlist);

/* ------- Private Globals ------------ */

static PyObject *expy_exim_dict = NULL;
static PyObject *expy_user_module = NULL;


/* ------- Custom type for holding header lines ------ 

  Basically an object with .text and .type attributes, only the
  .type attribute is changable, and only to single-character 
  values.  Usually it'd be '*' which Exim interprets as 
  meaning the line should be deleted.

  Also accessible for backwards compatibility as a sequence

*/

typedef struct
    {
    PyObject_HEAD
    header_line *hline;
    } expy_header_line_t;


static void expy_header_line_dealloc(PyObject *self)
    {
    PyObject_Del(self);
    }


static PyObject * expy_header_line_getattr(expy_header_line_t *self, char *name)
    {
    if (self->hline == NULL)
        {
        PyErr_Format(PyExc_AttributeError, "Header object no longer valid, held over from previously processed message?");
        return NULL;
        }

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
    if (self->hline == NULL)
        {
        PyErr_Format(PyExc_AttributeError, "Header object no longer valid, held over from previously processed message?");
        return NULL;
        }

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
    expy_header_line_dealloc,   /*tp_dealloc*/
    0,                          /*tp_print*/
    (getattrfunc) expy_header_line_getattr,  /*tp_getattr*/
    (setattrfunc) expy_header_line_setattr,  /*tp_setattr*/
    };


PyObject * expy_create_header_line(header_line *p)
    {
    expy_header_line_t * result;

    result = PyObject_NEW(expy_header_line_t, &ExPy_Header_Line);  /* New Reference */
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
 * of header objects.  
 *
 * Returns New reference
 */
static PyObject *get_headers()
    {
    int header_count;  
    header_line *p;
    PyObject *result;

    /* count number of header lines */
    for (header_count = 0, p = header_list; p; p = p->next)
        header_count++;

    /* Build up the tuple of tuples */
    result = PyTuple_New(header_count);           /* New reference */
    for (header_count = 0, p = header_list; p; p = p->next)
        {
        PyTuple_SetItem(result, header_count, expy_create_header_line(p));   /* Steals new reference */
        header_count++;
        }

    return result;
    }

/*
 * Given the header tuple created by get_headers(), go through
 * and set the header objects to point to NULL, in case someone
 * tries to re-use them after a message is done being processed, and
 * the underlying header strings are no longer available 
 */
static void clear_headers(PyObject *header_tuple)
    {
    int i, n;

    n = PyTuple_Size(header_tuple);
    for (i = 0; i < n; i++)
        {
        expy_header_line_t *p;

        p = (expy_header_line_t *) PyTuple_GetItem(header_tuple, i); /* Borrowed reference */
        p->hline = NULL;
        }

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
    PyObject *header_tuple;
    PyObject *original_recipients;
    PyObject *working_recipients;

    if (!expy_enabled)
        return LOCAL_SCAN_ACCEPT;

    if (!Py_IsInitialized())  /* local_scan() may have already been run */
        Py_Initialize();

    if (!expy_exim_dict)
        {
        PyObject *module = Py_InitModule(expy_exim_module, expy_exim_methods); /* Borrowed reference */
        Py_INCREF(module);                                 /* convert to New reference */
        expy_exim_dict = PyModule_GetDict(module);         /* Borrowed reference */
        Py_INCREF(expy_exim_dict);                         /* convert to New reference */
        }

    if (!expy_user_module)
        {
        if (expy_path_add)
            {
            PyObject *sys_module;
            PyObject *sys_dict;
            PyObject *sys_path;
            PyObject *add_value;

            sys_module = PyImport_ImportModule("sys");  /* New Reference */
            if (!sys_module)
                {
                PyErr_Clear();
                *return_text = "Internal error, can't import Python sys module";
                log_write(0, LOG_REJECT, "Couldn't import Python 'sys' module"); 
                /* FIXME: write out an exception traceback if possible to Exim log */
                return PYTHON_FAILURE_RETURN;
                }

            sys_dict = PyModule_GetDict(sys_module);               /* Borrowed Reference, never fails */
            sys_path = PyMapping_GetItemString(sys_dict, "path");  /* New reference */

            if (!sys_path || (!PyList_Check(sys_path)))
                {
                PyErr_Clear();  /* in case sys_path was NULL, harmless otherwise */
                *return_text = "Internal error, sys.path doesn't exist or isn't a list";
                log_write(0, LOG_REJECT, "expy: Python sys.path doesn't exist or isn't a list"); 
                /* FIXME: write out an exception traceback if possible to Exim log */
                return PYTHON_FAILURE_RETURN;
                }

            add_value = PyString_FromString(expy_path_add);  /* New reference */
            if (!add_value)
                {
                PyErr_Clear();
                log_write(0, LOG_PANIC, "expy: Failed to create Python string from [%s]", expy_path_add); 
                return PYTHON_FAILURE_RETURN;
                }

            if (PyList_Append(sys_path, add_value))
                {
                PyErr_Clear();
                log_write(0, LOG_PANIC, "expy: Failed to append [%s] to Python sys.path", expy_path_add);                
                }

            Py_DECREF(add_value);
            Py_DECREF(sys_path);
            Py_DECREF(sys_module);
            }

        expy_user_module = PyImport_ImportModule(expy_scan_module);  /* New Reference */

        if (!expy_user_module)
            {
            PyErr_Clear();
            *return_text = "Internal error, can't import Python local_scan module";
            log_write(0, LOG_REJECT, "Couldn't import Python '%s' module", expy_scan_module); 
            return PYTHON_FAILURE_RETURN;
            }
        }

    user_dict = PyModule_GetDict(expy_user_module);                      /* Borrowed Reference, never fails */
    user_func = PyMapping_GetItemString(user_dict, expy_scan_function);  /* New reference */

    if (!user_func)
        {
        PyErr_Clear();
        *return_text = "Internal error, module doesn't have local_scan function";
        log_write(0, LOG_REJECT, "Python %s module doesn't have a %s function", expy_scan_module, expy_scan_function); 
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
    header_tuple = get_headers();
    PyDict_SetItemString(expy_exim_dict, "headers", header_tuple);

    /* 
     * make list of recipients, give module a copy to work with in 
     * List format, but keep original tuple to compare against later
     */
    original_recipients = get_recipients();                     /* New reference */
    working_recipients = PySequence_List(original_recipients);  /* New reference */
    PyDict_SetItemString(expy_exim_dict, "recipients", working_recipients);
    Py_DECREF(working_recipients);    

    /* Try calling our function */
    result = PyObject_CallFunction(user_func, NULL);            /* New reference */

    Py_DECREF(user_func);  /* Don't need ref to function anymore */      

    /* Check for Python exception */
    if (!result)
        {
        PyErr_Clear();
        *return_text = "Internal error, local_scan function failed";
        Py_DECREF(original_recipients);
        clear_headers(header_tuple);
        Py_DECREF(header_tuple);
        return PYTHON_FAILURE_RETURN;

        // FIXME: should write exception to exim log somehow
        }

    /* User code may have replaced recipient list, so re-get ref */
    working_recipients = PyDict_GetItemString(expy_exim_dict, "recipients"); /* Borrowed reference */
    Py_XINCREF(working_recipients);                                           /* convert to New reference */

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

    Py_XDECREF(working_recipients);   /* No longer needed */
    Py_DECREF(original_recipients);   /* No longer needed */

    clear_headers(header_tuple);
    Py_DECREF(header_tuple);          /* No longer needed */

    /* Deal with the return value, first see if python returned a non-empty sequence */
    if (PySequence_Check(result) && (PySequence_Size(result) > 0))
        {
        /* save first item */
        PyObject *rc = PySequence_GetItem(result, 0);

        /* if more than one item, convert 2nd item to string and use as return text */
        if (PySequence_Size(result) > 1)
            {
            PyObject *str;
            PyObject *obj = PySequence_GetItem(result, 1);   /* New reference */
            str = PyObject_Str(obj);                         /* New reference */

            *return_text = string_copy(PyString_AsString(str));

            Py_DECREF(obj);
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
    log_write(0, LOG_REJECT, "Python %s.%s function didn't return integer", expy_scan_module, expy_scan_function); 
    return PYTHON_FAILURE_RETURN;
    }


/*-------- EOF --------------*/
