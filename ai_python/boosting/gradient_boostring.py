import numpy as np
from sklearn import datasets
import matplotlib.pyplot as plt
from sklearn.metrics import accuracy_score
from sklearn.model_selection import train_test_split
from sklearn.tree import DecisionTreeClassifier
from sklearn.base import clone

class GradientBoosting: 
    def __init__(self, base_learner, n_learners, learning_rate): 
        self.learners = [clone(base_learner) for _ in range(n_learners)] 
        self.lr = learning_rate 
         
    def fit(self, X, y): 
        residual = y.copy() 
        for learner in self.learners: 
            learner.fit(X, residual) 
            residual -= self.lr * learner.predict(X)           
             
    def predict(self,X): 
        preds = [learner.predict(X) for learner in self.learners] 
        return np.array(preds).sum(axis=0) * self.lr

if __name__ == '__main__':
    data = datasets.load_breast_cancer()
    X = data.data
    y = data.target
    y = np.stack([(y==0).astype(int), (y==1).astype(int)]).T
    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.4)
    clf = GradientBoosting(base_learner=DecisionTreeClassifier(max_depth=2, random_state=1), n_learners=5, learning_rate=0.1)
    clf.fit(X_train, y_train)
    print(accuracy_score(np.argmax(y_test, axis=1), clf.predict(X_test)))