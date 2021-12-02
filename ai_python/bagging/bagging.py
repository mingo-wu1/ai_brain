import numpy as np
from sklearn.base import clone

class Bagging: 
    def __init__(self, base_learner, n_learners): 
        self.learners = [clone(base_learner) for _ in range(n_learners)] 
         
    def fit(self, X, y): 
        for learner in self.learners: 
            examples = np.random.choice( 
                np.arange(len(X)), int(len(X)), replace=True) 
            learner.fit(X.iloc[examples, :], y.iloc[examples])
             
    def predict(self, X): 
        preds = [learner.predict(X) for learner in self.learners] 
        return np.array(preds).mean(axis=0)