#include <stdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#ifndef LIBXML_TREE_ENABLED
#error "Installed Libxml2 does not support tree functionality!"
#endif

/*
 *To compile this file using gcc you can type
 *gcc `xml2-config --cflags --libs` -o xmlexample libxml2-example.c
 */

static void
print_element_names(xmlNode * a_node);

/**
 * print_attribute_names:
 * @a_node: the initial xml node to consider.
 *
 * Prints the names of the all the xml attributes
 * that are siblings or children of a given xml node.
 */
static void
print_attribute_names(xmlAttr * a_node)
{
    xmlAttr *cur_node = NULL;

    for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ATTRIBUTE_NODE) {
            printf("   node type: Attribute, name: %s\n", cur_node->name);
        }
        else {
            printf("   ATTR: UNKNOWN NODE TYPE: %s\n", cur_node->name);
        }
        print_element_names(cur_node->children);

    }
}



/**
 * print_element_names:
 * @a_node: the initial xml node to consider.
 *
 * Prints the names of the all the xml elements
 * that are siblings or children of a given xml node.
 */
static void
print_element_names(xmlNode * a_node)
{
    xmlNode *cur_node = NULL;

    for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE) {
            printf("node type: Element, name: %s\n", cur_node->name);
        }
        else if (cur_node->type == XML_TEXT_NODE) {
            printf("      text = \"%s\"\n", cur_node->content );
        }
        else {
            printf("   UNKNOWN NODE TYPE: %s\n", cur_node->name);
        }

        printf( "                 attributes of node...\n");
        print_attribute_names(cur_node->properties);
        printf( "                 children of node...\n");
        print_element_names(cur_node->children);
        printf( "                 next node...\n");
    }
    printf("                 ...done\n");
}


/**
 * Simple example to parse a file called "file.xml", 
 * walk down the DOM, and print the name of the 
 * xml elements nodes.
 */
int
main(int argc, char **argv)
{
    xmlDoc *doc = NULL;
    xmlNode *root_element = NULL;

    if (argc != 2)
        return(1);

    /*
     * this initialize the library and check potential ABI mismatches
     * between the version it was compiled for and the actual shared
     * library used.
     */
    LIBXML_TEST_VERSION

        /*parse the file and get the DOM */
    doc = xmlReadFile(argv[1], NULL, XML_PARSE_NOBLANKS);

    if (doc == NULL) {
        printf("error: could not parse file %s\n", argv[1]);
    }

    /*Get the root element node */
    root_element = xmlDocGetRootElement(doc);

    print_element_names(root_element);

    /*free the document */
    xmlFreeDoc(doc);

    /*
     *Free the global variables that may
     *have been allocated by the parser.
     */
    xmlCleanupParser();

    return 0;
}

