#ifndef B_TREE
#define B_TREE

#include <iostream>
#include <stdio.h>

/**
 * @brief Balanced multiple search tree
 */
class Balanced_Multiple_Search_Tree {
public:
    /**
     * @brief Constructor
     */
    Balanced_Multiple_Search_Tree(){
        root = NULL;
    }

    /**
     * @brief Destructor
     */
    ~Balanced_Multiple_Search_Tree(){
        Destroy(root);
    }

    /**
     * @brief Insert a new node
     * @param key Key
     * @param value Value
     */
    void insert(int key, int value){
        if(root == NULL){
            root = new Node(key, value);
        }
        else{
            insert(root, key, value);
        }
    }

    /**
     * @brief Search a node
     * @param key Key
     * @return Value
     */
    int search(int key){
        if(root == NULL){
            return -1;
        }
        else{
            return search(root, key);
        }
    }

    /**
     * @brief Delete a node
     * @param key Key
     */
    void delete_node(int key){
        if(root == NULL){
            return;
        }
        else{
            delete_node(root, key);
        }
    }

    /**
     * @brief Print the tree
     */
    void print(){
        if(root == NULL){
            std::cout << "Empty tree" << std::endl;
        }
        else{
            print(root);
        }
    }
    
private:
    /**
     * @brief Node
     */
    struct Node {
        int key;
        int value;
        Node *left;
        Node *right;
        Node *parent;
        Node(int key, int value) : key(key), value(value), left(NULL), right(NULL), parent(NULL) {}
    };

    /**
     * @brief Root node
     */
    Node *root;

    /**
     * @brief Insert a new node
     * @param key Key
     * @param value Value
     */
    void insert(Node *node, int key, int value){
        if(node->key == key){
            node->value = value;
        }
        else if(node->key > key){
            if(node->left == NULL){
                node->left = new Node(key, value);
                node->left->parent = node;
            }
            else{
                insert(node->left, key, value);
            }
        }
        else{
            if(node->right == NULL){
                node->right = new Node(key, value);
                node->right->parent = node;
            }
            else{
                insert(node->right, key, value);
            }
        }
    }

    /**
     * @brief Search a node
     * @param key Key
     * @return Value
     */
    int search(Node *node, int key){
        if(node == NULL){
            return -1;
        }
        else if(node->key == key){
            return node->value;
        }
        else if(node->key > key){
            return search(node->left, key);
        }
        else{
            return search(node->right, key);
        }
    }

    /**
     * @brief Delete a node
     * @param key Key
     */
    void delete_node(Node *node, int key){
        if(node == NULL){
            return;
        }
        else if(node->key == key){
            if(node->left == NULL && node->right == NULL){
                if(node->parent->left == node){
                    node->parent->left = NULL;
                }
                else{
                    node->parent->right = NULL;
                }
                delete node;
            }
            else if(node->left == NULL){
                if(node->parent->left == node){
                    node->parent->left = node->right;
                    node->right->parent = node->parent;
                }
                else{
                    node->parent->right = node->right;
                    node->right->parent = node->parent;
                }
                delete node;
            }
            else if(node->right == NULL){
                if(node->parent->left == node){
                    node->parent->left = node->left;
                    node->left->parent = node->parent;
                }
                else{
                    node->parent->right = node->left;
                    node->left->parent = node->parent;
                }
                delete node;
            }
            else{
                Node *temp = node->right;
                while(temp->left != NULL){
                    temp = temp->left;
                }
                node->key = temp->key;
                node->value = temp->value;
                delete_node(node->right, temp->key);
            }
        }
        else if(node->key > key){
            delete_node(node->left, key);
        }
        else{
            delete_node(node->right, key);
        }
    }


    void Destroy(Node *node){
        if(node == NULL)
            return;
        Destroy(node->left);
        Destroy(node->right);
        delete node;
    }

    /**
     * @brief Print the tree
     */
    void print(Node *node){
        if(node == NULL){
            return;
        }
        else{
            print(node->left);
            std::cout << node->key << " " << node->value << std::endl;
            print(node->right);
        }
    }
};
#endif
